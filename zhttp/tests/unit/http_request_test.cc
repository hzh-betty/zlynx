#include "zhttp/http_request.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

TEST(HttpRequestTest, DefaultValues) {
    HttpRequest req;
    EXPECT_EQ(req.method(), HttpMethod::UNKNOWN);
    EXPECT_EQ(req.path(), "");
    EXPECT_EQ(req.version(), HttpVersion::HTTP_1_1);
    EXPECT_TRUE(req.body().empty());
}

TEST(HttpRequestTest, SettersAndGetters) {
    HttpRequest req;
    req.set_method(HttpMethod::POST);
    req.set_path("/api/users");
    req.set_query("id=123");
    req.set_version(HttpVersion::HTTP_1_0);
    req.set_body("{\"name\":\"test\"}");

    EXPECT_EQ(req.method(), HttpMethod::POST);
    EXPECT_EQ(req.path(), "/api/users");
    EXPECT_EQ(req.query(), "id=123");
    EXPECT_EQ(req.version(), HttpVersion::HTTP_1_0);
    EXPECT_EQ(req.body(), "{\"name\":\"test\"}");
}

TEST(HttpRequestTest, HeadersCaseInsensitive) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json");
    req.set_header("X-Custom-Header", "value");

    EXPECT_EQ(req.header("content-type"), "application/json");
    EXPECT_EQ(req.header("CONTENT-TYPE"), "application/json");
    EXPECT_EQ(req.header("x-custom-header"), "value");
    EXPECT_EQ(req.header("NonExistent", "default"), "default");
}

TEST(HttpRequestTest, HeaderOverwriteUsesLatestValueCaseInsensitively) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json");
    req.set_header("content-type", "text/plain");

    EXPECT_EQ(req.header("Content-Type"), "text/plain");
}

TEST(HttpRequestTest, PathParams) {
    HttpRequest req;
    req.set_path_param("id", "123");
    req.set_path_param("name", "test");

    EXPECT_EQ(req.path_param("id"), "123");
    EXPECT_EQ(req.path_param("name"), "test");
    EXPECT_EQ(req.path_param("unknown", "default"), "default");
}

TEST(HttpRequestTest, ParseQueryParams) {
    HttpRequest req;
    req.set_query("name=John&age=30&city=Beijing");
    req.parse_query_params();

    EXPECT_EQ(req.query_param("name"), "John");
    EXPECT_EQ(req.query_param("age"), "30");
    EXPECT_EQ(req.query_param("city"), "Beijing");
}

TEST(HttpRequestTest, ParseQueryParamsWithUrlEncoding) {
    HttpRequest req;
    req.set_query("name=John%20Doe&msg=Hello+World");
    req.parse_query_params();

    EXPECT_EQ(req.query_param("name"), "John Doe");
    EXPECT_EQ(req.query_param("msg"), "Hello World");
}

TEST(HttpRequestTest, ParseQueryParamsHandlesEmptySegmentsAndClearsOnReparse) {
    HttpRequest req;
    req.set_query("a=1&&flag&k=&encoded=%2B+");
    req.parse_query_params();

    EXPECT_EQ(req.query_param("a"), "1");
    EXPECT_EQ(req.query_param("flag"), "");
    EXPECT_EQ(req.query_param("k"), "");
    EXPECT_EQ(req.query_param("encoded"), "+ ");

    req.set_query("");
    req.parse_query_params();
    EXPECT_EQ(req.query_param("a", "none"), "none");
}

TEST(HttpRequestTest, KeepAliveHttp11Default) {
    HttpRequest req;
    req.set_version(HttpVersion::HTTP_1_1);
    EXPECT_TRUE(req.is_keep_alive());

    req.set_header("Connection", "close");
    EXPECT_FALSE(req.is_keep_alive());
}

TEST(HttpRequestTest, KeepAliveHttp10Default) {
    HttpRequest req;
    req.set_version(HttpVersion::HTTP_1_0);
    EXPECT_FALSE(req.is_keep_alive());

    req.set_header("Connection", "keep-alive");
    EXPECT_TRUE(req.is_keep_alive());
}

TEST(HttpRequestTest, ContentLength) {
    HttpRequest req;
    EXPECT_EQ(req.content_length(), 0u);

    req.set_header("Content-Length", "1024");
    EXPECT_EQ(req.content_length(), 1024u);
}

TEST(HttpRequestTest, ContentLengthInvalidValueFallsBackToZero) {
    HttpRequest req;
    req.set_header("Content-Length", "invalid");
    EXPECT_EQ(req.content_length(), 0u);
}

TEST(HttpRequestTest, ParseJsonBodySuccess) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json; charset=utf-8");
    req.set_body("{\"name\":\"betty\",\"age\":18}");

    EXPECT_TRUE(req.is_json());
    EXPECT_TRUE(req.parse_json());

    const HttpRequest::Json *json = req.json();
    ASSERT_NE(json, nullptr);
    EXPECT_EQ((*json)["name"], "betty");
    EXPECT_EQ((*json)["age"], 18);
}

TEST(HttpRequestTest, ParseJsonBodyInvalid) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json");
    req.set_body("{\"name\":\"broken\"");

    EXPECT_FALSE(req.parse_json());
    EXPECT_EQ(req.json(), nullptr);
    EXPECT_FALSE(req.json_error().empty());
}

TEST(HttpRequestTest, ParseJsonCoversNonJsonEmptyBodyAndCachedFailure) {
    HttpRequest req;
    req.set_header("Content-Type", "text/plain");
    req.set_body("plain");
    EXPECT_TRUE(req.parse_json());
    EXPECT_EQ(req.json(), nullptr);
    EXPECT_TRUE(req.json_error().empty());

    req.set_header("Content-Type", "application/json");
    req.set_body("");
    EXPECT_FALSE(req.parse_json());
    EXPECT_EQ(req.json(), nullptr);
    EXPECT_EQ(req.json_error(), "Empty JSON body");

    // 第二次调用直接命中缓存失败分支。
    EXPECT_FALSE(req.parse_json());
    EXPECT_EQ(req.json_error(), "Empty JSON body");
}

TEST(HttpRequestTest, ParseFormUrlencodedBody) {
    HttpRequest req;
    req.set_header("Content-Type", "application/x-www-form-urlencoded");
    req.set_body("name=John+Doe&city=ShenZhen&flag");

    EXPECT_TRUE(req.is_form_urlencoded());
    EXPECT_TRUE(req.parse_form_urlencoded());
    EXPECT_EQ(req.form_param("name"), "John Doe");
    EXPECT_EQ(req.form_param("city"), "ShenZhen");
    EXPECT_EQ(req.form_param("flag"), "");
    EXPECT_EQ(req.form_param("missing", "default"), "default");
}

TEST(HttpRequestTest, ParseFormUrlencodedCoversNonFormEmptyAndCache) {
    HttpRequest req;
    req.set_header("Content-Type", "text/plain");
    req.set_body("name=ignored");
    EXPECT_TRUE(req.parse_form_urlencoded());
    EXPECT_TRUE(req.form_params().empty());

    req.set_header("Content-Type", "application/x-www-form-urlencoded");
    req.set_body("");
    EXPECT_TRUE(req.parse_form_urlencoded());
    EXPECT_TRUE(req.form_params().empty());
    EXPECT_TRUE(req.parse_form_urlencoded());

    req.set_body("name=Alice&flag");
    EXPECT_TRUE(req.parse_form_urlencoded());
    EXPECT_EQ(req.form_param("name"), "Alice");
    EXPECT_EQ(req.form_param("flag"), "");
}

TEST(HttpRequestTest, CookieParsingHandlesFlagsAndMalformedPairs) {
    HttpRequest req;
    req.set_header(
        "Cookie",
        "a=1; theme=dark ; flag ; =skip ; ; spaced = value ;multi=a=b");

    EXPECT_EQ(req.cookie("a"), "1");
    EXPECT_EQ(req.cookie("theme"), "dark");
    EXPECT_EQ(req.cookie("flag"), "");
    EXPECT_EQ(req.cookie("spaced"), "value");
    EXPECT_EQ(req.cookie("multi"), "a=b");
    EXPECT_EQ(req.cookie("missing", "default"), "default");

    const auto &cookies = req.cookies();
    EXPECT_EQ(cookies.count(""), 0u);

    HttpRequest no_cookie;
    EXPECT_TRUE(no_cookie.cookies().empty());
    EXPECT_EQ(no_cookie.cookie("none", "fallback"), "fallback");
}

TEST(HttpRequestTest,
     ParseMultipartCoversFailureCacheAndContentTypeInvalidation) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json");
    req.set_body("{}");
    EXPECT_TRUE(req.parse_multipart());
    EXPECT_EQ(req.multipart(), nullptr);

    req.set_header("Content-Type", "multipart/form-data; boundary=b");
    req.set_body("no-boundary");
    EXPECT_FALSE(req.parse_multipart());
    EXPECT_EQ(req.multipart(), nullptr);
    EXPECT_FALSE(req.multipart_error().empty());
    EXPECT_FALSE(req.parse_multipart());

    req.set_header("Content-Type", "application/json");
    EXPECT_TRUE(req.parse_multipart());
    EXPECT_EQ(req.multipart(), nullptr);
    EXPECT_TRUE(req.multipart_error().empty());
}

TEST(HttpRequestTest, ResetBodyInvalidatesBodyParseCache) {
    HttpRequest req;
    req.set_header("Content-Type", "application/json");
    req.set_body("{\"v\":1}");
    ASSERT_TRUE(req.parse_json());
    ASSERT_NE(req.json(), nullptr);

    req.set_body("{\"v\":2}");
    ASSERT_TRUE(req.parse_json());
    ASSERT_NE(req.json(), nullptr);
    EXPECT_EQ((*req.json())["v"], 2);
}

TEST(HttpRequestTest, LazyRemoteAddrResolverRunsOnlyOnce) {
    HttpRequest req;
    int calls = 0;
    req.set_remote_addr_resolver([&calls]() {
        ++calls;
        return std::string("127.0.0.1:8080");
    });

    EXPECT_EQ(req.remote_addr(), "127.0.0.1:8080");
    EXPECT_EQ(req.remote_addr(), "127.0.0.1:8080");
    EXPECT_EQ(calls, 1);
}

TEST(HttpRequestTest, RemoteAddrSettersAndFallbackPaths) {
    HttpRequest req;
    EXPECT_EQ(req.remote_addr(), "");

    req.set_remote_addr(std::string("10.0.0.1:1000"));
    EXPECT_EQ(req.remote_addr(), "10.0.0.1:1000");

    int calls = 0;
    req.set_remote_addr_resolver([&calls]() {
        ++calls;
        return std::string("192.168.1.10:8080");
    });
    EXPECT_EQ(req.remote_addr(), "192.168.1.10:8080");
    EXPECT_EQ(req.remote_addr(), "192.168.1.10:8080");
    EXPECT_EQ(calls, 1);
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
