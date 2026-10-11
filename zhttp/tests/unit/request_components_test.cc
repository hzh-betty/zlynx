#include "protocol/http/response_encoder.h"
#include "../support/request_builder.h"
#include "zhttp/content/multipart.h"
#include <type_traits>
using namespace zhttp;
TEST(RequestBodyTest, MultipartCacheUsesInjectedParserAndInvalidatesTogether) {
    std::string bytes = "payload";
    detail::ParsedRequestBody body(bytes);
    int calls = 0;
    auto result = std::make_shared<MultipartFormData>();
    detail::ParsedRequestBody::MultipartParser parser = [&](std::string *) {
        ++calls;
        return result;
    };
    EXPECT_EQ(body.multipart("multipart/form-data", parser), result.get());
    EXPECT_TRUE(body.parse_multipart("multipart/form-data", parser));
    EXPECT_EQ(calls, 1);
    bytes = "next payload";
    body.invalidate();
    EXPECT_EQ(body.multipart("multipart/form-data", parser), result.get());
    EXPECT_EQ(calls, 2);
    body.invalidate();
    detail::ParsedRequestBody::MultipartParser failed = [&](std::string *error) {
        ++calls;
        *error = "missing boundary";
        return std::shared_ptr<MultipartFormData>{};
    };
    EXPECT_FALSE(body.parse_multipart("multipart/form-data", failed));
    EXPECT_FALSE(body.parse_multipart("multipart/form-data", failed));
    EXPECT_EQ(body.multipart_error(), "missing boundary");
    EXPECT_EQ(calls, 3);
}
TEST(ProtocolModelTest, ComposedLinesAndEncodedTargetsHaveOneSource) {
    HttpRequest request;
    request.set_method(HttpMethod::GET);
    request.set_target("http://example.com/a%2Fb?q=one&q=two");
    EXPECT_EQ(request.method(), request.request_line().method);
    EXPECT_EQ(request.uri().form(), Uri::Form::Absolute);
    EXPECT_EQ(request.uri().raw(), "http://example.com/a%2Fb?q=one&q=two");
    EXPECT_EQ(request.path(), "/a%2Fb");
    EXPECT_EQ(request.query_param("q"), "one");
    EXPECT_EQ(request.query_values("q"),
              (std::vector<std::string>{"one", "two"}));
    HttpResponse response;
    response.status(299);
    EXPECT_EQ(response.status_line().status_code, 299);
    EXPECT_EQ(ResponseEncoder::serialize(response).substr(0, 15),
              "HTTP/1.1 299 \r\n");
    EXPECT_THROW(request.set_target("/%GG"), std::invalid_argument);
}
TEST(ProtocolModelTest, HeadersPreserveOrderAndDuplicatesAndRejectInjection) {
    HttpHeaders headers;
    headers.append("Set-Cookie", "a=1");
    headers.append("set-cookie", "b=2");
    EXPECT_EQ(headers.get("SET-cookie"), "a=1");
    EXPECT_EQ(headers.get_all("set-cookie"),
              (std::vector<std::string>{"a=1", "b=2"}));
    headers.set("SET-COOKIE", "c=3");
    EXPECT_EQ(headers.size(), 1u);
    EXPECT_THROW(headers.append("Bad Name", "x"), std::invalid_argument);
    EXPECT_THROW(headers.set("X", "ok\r\nBad: header"), std::invalid_argument);
}
TEST(ProtocolModelTest, HeaderViewsDistinguishMissingAndEmptyAndInputsAreOwned) {
    HttpHeaders headers;
    EXPECT_FALSE(headers.get_view("Missing").has_value());
    headers.append("Empty", "");
    ASSERT_TRUE(headers.get_view("empty").has_value());
    EXPECT_TRUE(headers.get_view("empty")->empty());

    std::string name = "[X-Long-Header-Name]";
    std::string value = "[" + std::string(256, 'v') + "]";
    headers.append(std::string_view(name).substr(1, name.size() - 2),
                   std::string_view(value).substr(1, value.size() - 2));
    name.assign(name.size(), 'x');
    value.assign(value.size(), 'x');
    EXPECT_EQ(headers.get_view("x-long-header-name"), std::string(256, 'v'));
    EXPECT_EQ(headers.count("X-Long-Header-Name"), 1u);
}
TEST(ProtocolModelTest, HeaderMutationsAcceptViewsIntoTheirOwnFields) {
    HttpHeaders headers;
    headers.append("X-Long-Header-Name", std::string(256, 'a'));
    for (int i = 0; i < 64; ++i) {
        const auto &first = *headers.begin();
        headers.append(first.first, first.second);
    }
    EXPECT_EQ(headers.count("x-long-header-name"), 65u);
    const auto &first = *headers.begin();
    headers.set(first.first, first.second);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers.get("X-Long-Header-Name"), std::string(256, 'a'));

    headers.append("Other-Long-Header", "kept");
    headers.append("X-Long-Header-Name", "second");
    headers.erase(headers.begin()->first);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers.begin()->first, "Other-Long-Header");
    EXPECT_EQ(headers.begin()->second, "kept");
}
TEST(ProtocolModelTest, QueryLookupPreservesDecodingDuplicatesAndEmptyValues) {
    Uri uri("/?a+b=one&a%20b=two&flag&empty=&binary=%00%ff&&=blank");
    const std::string key = "[a b]";
    const auto bounded_key = std::string_view(key).substr(1, 3);
    EXPECT_EQ(uri.query_param(bounded_key), "one");
    EXPECT_EQ(uri.query_values(bounded_key),
              (std::vector<std::string>{"one", "two"}));
    EXPECT_EQ(uri.query_param("flag", "fallback"), "");
    EXPECT_EQ(uri.query_param("empty", "fallback"), "");
    EXPECT_EQ(uri.query_param("missing", "fallback"), "fallback");
    EXPECT_EQ(uri.query_param(""), "blank");
    EXPECT_EQ(uri.query_param("binary"), std::string("\0\xff", 2));
}
TEST(ProtocolModelTest, BodyMovesResourcesAndModelIsNotImplicitlyCopied) {
    static_assert(!std::is_copy_constructible<HttpBody>::value,
                  "body resource cannot be copied");
    static_assert(!std::is_copy_constructible<HttpRequest>::value,
                  "request owns body");
    auto body = HttpBody::memory("bytes");
    auto moved = std::move(body);
    EXPECT_EQ(body.kind(), HttpBody::Kind::Empty);
    EXPECT_EQ(moved.content(), "bytes");
    EXPECT_EQ(moved.length(), 5u);
}
TEST(ContextTest, CompletionRunsOnceAndSurvivesCallbackExceptions) {
    auto request = std::make_shared<HttpRequest>();
    HttpContext context(request);
    int calls = 0;
    context.on_complete([&](CompletionResult result) {
        ++calls;
        EXPECT_EQ(result, CompletionResult::Completed);
        throw std::runtime_error("callback");
    });
    context.on_complete([&](CompletionResult) { ++calls; });
    context.complete(CompletionResult::Completed);
    context.complete(CompletionResult::Cancelled);
    EXPECT_EQ(calls, 2);
}
TEST(RequestComponentsTest, ReadonlyContextOwnsMutableDerivedCaches) {
    auto json = std::make_shared<HttpRequest>();
    json->set_header("Content-Type", "application/json");
    json->set_body("{\"value\":42}");
    const HttpContext context(json);
    ASSERT_NE(context.json(), nullptr);
    EXPECT_EQ(context.json()->at("value"), 42);
    EXPECT_EQ(context.json(), context.json());
    auto form = std::make_shared<HttpRequest>();
    form->set_header("Content-Type", "application/x-www-form-urlencoded");
    form->set_body("key=value");
    const HttpContext second(form);
    EXPECT_EQ(second.form_param("key"), "value");
    EXPECT_EQ(second.form_params().size(), 1u);
}
