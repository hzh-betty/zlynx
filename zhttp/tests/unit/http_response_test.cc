#include "zhttp/http_response.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

TEST(HttpResponseTest, DefaultValues) {
    HttpResponse resp;
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
    EXPECT_TRUE(resp.is_keep_alive());
    EXPECT_TRUE(resp.body_content().empty());
}

TEST(HttpResponseTest, ChainedSetters) {
    HttpResponse resp;
    resp.status(HttpStatus::CREATED)
        .header("X-Custom", "value")
        .content_type("text/plain")
        .body("Hello World");

    EXPECT_EQ(resp.status_code(), HttpStatus::CREATED);
    EXPECT_EQ(resp.headers().at("X-Custom"), "value");
    EXPECT_EQ(resp.headers().at("Content-Type"), "text/plain");
    EXPECT_EQ(resp.body_content(), "Hello World");
}

TEST(HttpResponseTest, JsonResponse) {
    HttpResponse resp;
    resp.json("{\"key\":\"value\"}");

    EXPECT_EQ(resp.headers().at("Content-Type"),
              "application/json; charset=utf-8");
    EXPECT_EQ(resp.body_content(), "{\"key\":\"value\"}");
}

TEST(HttpResponseTest, HtmlResponse) {
    HttpResponse resp;
    resp.html("<h1>Hello</h1>");

    EXPECT_EQ(resp.headers().at("Content-Type"), "text/html; charset=utf-8");
    EXPECT_EQ(resp.body_content(), "<h1>Hello</h1>");
}

TEST(HttpResponseTest, TextResponse) {
    HttpResponse resp;
    resp.text("Hello World");

    EXPECT_EQ(resp.headers().at("Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(resp.body_content(), "Hello World");
}

TEST(HttpResponseTest, Redirect) {
    HttpResponse resp;
    resp.redirect("https://example.com", HttpStatus::MOVED_PERMANENTLY);

    EXPECT_EQ(resp.status_code(), HttpStatus::MOVED_PERMANENTLY);
    EXPECT_EQ(resp.headers().at("Location"), "https://example.com");
    EXPECT_TRUE(resp.body_content().empty());
}

TEST(HttpResponseTest, Serialize) {
    HttpResponse resp;
    resp.status(HttpStatus::OK).content_type("text/plain").body("Hello");
    resp.set_keep_alive(true);

    std::string serialized = resp.serialize();

    // 检查状态行
    EXPECT_NE(serialized.find("HTTP/1.1 200 OK"), std::string::npos);
    // 检查 Content-Type
    EXPECT_NE(serialized.find("Content-Type: text/plain"), std::string::npos);
    // 检查 Content-Length
    EXPECT_NE(serialized.find("Content-Length: 5"), std::string::npos);
    // 检查 Connection
    EXPECT_NE(serialized.find("Connection: keep-alive"), std::string::npos);
    // 检查 body
    EXPECT_NE(serialized.find("\r\n\r\nHello"), std::string::npos);
}

TEST(HttpResponseTest, StatusCodeInteger) {
    HttpResponse resp;
    resp.status(404);

    EXPECT_EQ(resp.status_code(), HttpStatus::NOT_FOUND);
}

TEST(HttpResponseTest, ChunkedResponseOmitsContentLength) {
    HttpResponse resp;
    resp.status(HttpStatus::OK)
        .content_type("text/plain")
        .body("Hello")
        .enable_chunked();

    std::string serialized = resp.serialize();

    EXPECT_NE(serialized.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_EQ(serialized.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(serialized.find("\r\n\r\nHello"), std::string::npos);
}

TEST(HttpResponseTest, StreamCallbackEnablesChunked) {
    HttpResponse resp;
    bool called = false;
    resp.stream([&called](char *buffer, size_t size) -> size_t {
        called = true;
        if (size < 2) {
            return 0;
        }
        buffer[0] = 'o';
        buffer[1] = 'k';
        return 2;
    });

    std::string serialized = resp.serialize();

    EXPECT_TRUE(resp.has_stream_callback());
    EXPECT_TRUE(resp.is_chunked_enabled());
    EXPECT_NE(serialized.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_EQ(serialized.find("Content-Length:"), std::string::npos);
    EXPECT_FALSE(called);
}

TEST(HttpResponseTest, AsyncStreamCallbackEnablesChunked) {
    HttpResponse resp;
    bool called = false;
    resp.async_stream(
        [&called](HttpResponse::AsyncChunkSender,
                  HttpResponse::AsyncStreamCloser) { called = true; });

    std::string serialized = resp.serialize();

    EXPECT_TRUE(resp.has_async_stream_callback());
    EXPECT_TRUE(resp.is_chunked_enabled());
    EXPECT_FALSE(resp.is_keep_alive());
    EXPECT_NE(serialized.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_EQ(serialized.find("Content-Length:"), std::string::npos);
    EXPECT_FALSE(called);
}

TEST(HttpResponseTest, NoBodyStatusDoesNotEmitChunkedBody) {
    HttpResponse resp;
    resp.status(HttpStatus::NO_CONTENT).body("ignored").enable_chunked();

    std::string serialized = resp.serialize();

    EXPECT_EQ(serialized.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_EQ(serialized.find("\r\n\r\nignored"), std::string::npos);
}

TEST(HttpResponseTest, SerializeToMatchesSerializeOutput) {
    HttpResponse resp;
    resp.status(HttpStatus::CREATED)
        .header("X-Test", "value")
        .content_type("text/plain")
        .body("payload");
    resp.set_keep_alive(false);

    std::string serialized;
    resp.serialize_to(&serialized);

    EXPECT_EQ(serialized, resp.serialize());
}

TEST(HttpResponseTest, NoBodyStatusesDoNotEmitBodyOrContentLength) {
    HttpResponse no_content;
    no_content.status(HttpStatus::NO_CONTENT).body("ignored");
    const std::string s1 = no_content.serialize();
    EXPECT_EQ(s1.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(s1.find("\r\n\r\nignored"), std::string::npos);

    HttpResponse not_modified;
    not_modified.status(HttpStatus::NOT_MODIFIED).body("ignored");
    const std::string s2 = not_modified.serialize();
    EXPECT_EQ(s2.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(s2.find("\r\n\r\nignored"), std::string::npos);

    HttpResponse switching;
    switching.status(HttpStatus::SWITCHING_PROTOCOLS).body("ignored");
    const std::string s3 = switching.serialize();
    EXPECT_EQ(s3.find("Content-Length:"), std::string::npos);
    EXPECT_EQ(s3.find("\r\n\r\nignored"), std::string::npos);
}

TEST(HttpResponseTest, Http10DoesNotUseChunkedTransportEncoding) {
    HttpResponse resp;
    resp.set_version(HttpVersion::HTTP_1_0);
    resp.status(HttpStatus::OK).body("legacy").enable_chunked();

    const std::string serialized = resp.serialize();
    EXPECT_EQ(serialized.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_NE(serialized.find("Content-Length: 6"), std::string::npos);
    EXPECT_NE(serialized.find("\r\n\r\nlegacy"), std::string::npos);
}

TEST(HttpResponseTest, SerializeHandlesConnectionAndTransferEncodingHeaders) {
    HttpResponse chunked_resp;
    chunked_resp.status(HttpStatus::OK)
        .body("abc")
        .enable_chunked()
        .header("Content-Length", "999")
        .header("Transfer-Encoding", "gzip")
        .header("Connection", "close");
    const std::string chunked = chunked_resp.serialize();
    EXPECT_EQ(chunked.find("Content-Length:"), std::string::npos);
    EXPECT_NE(chunked.find("Transfer-Encoding: gzip"), std::string::npos);
    EXPECT_NE(chunked.find("Connection: close"), std::string::npos);

    HttpResponse no_body_resp;
    no_body_resp.status(HttpStatus::NO_CONTENT)
        .header("Transfer-Encoding", "chunked")
        .body("ignored");
    const std::string no_body = no_body_resp.serialize();
    EXPECT_EQ(no_body.find("Transfer-Encoding:"), std::string::npos);
}

TEST(HttpResponseTest, DisableChunkedClearsStreamCallbacks) {
    HttpResponse resp;
    resp.stream([](char *, size_t) { return 0U; });
    ASSERT_TRUE(resp.has_stream_callback());
    ASSERT_TRUE(resp.is_chunked_enabled());

    resp.enable_chunked(false);
    EXPECT_FALSE(resp.has_stream_callback());
    EXPECT_FALSE(resp.has_async_stream_callback());
    EXPECT_FALSE(resp.is_chunked_enabled());
}

TEST(HttpResponseTest, UpgradeToWebSocketClearsStreamingStateAndBody) {
    HttpResponse resp;
    resp.status(HttpStatus::OK)
        .body("payload")
        .stream([](char *, size_t) { return 0U; });

    WebSocketCallbacks callbacks;
    WebSocketOptions options;
    options.max_message_size = 4096;
    resp.upgrade_to_websocket(callbacks, options);

    EXPECT_TRUE(resp.has_websocket_upgrade());
    EXPECT_EQ(resp.status_code(), HttpStatus::SWITCHING_PROTOCOLS);
    EXPECT_TRUE(resp.is_keep_alive());
    EXPECT_FALSE(resp.is_chunked_enabled());
    EXPECT_FALSE(resp.has_stream_callback());
    EXPECT_FALSE(resp.has_async_stream_callback());
    EXPECT_TRUE(resp.body_content().empty());
    EXPECT_EQ(resp.websocket_options().max_message_size, 4096U);
}

TEST(HttpResponseTest, CookieFormattingCoversOptionalAttributes) {
    HttpResponse resp;
    HttpResponse::CookieOptions minimal_opt;
    minimal_opt.path = "";
    minimal_opt.max_age = -1;
    minimal_opt.http_only = false;
    minimal_opt.secure = false;
    minimal_opt.same_site = "";

    resp.set_cookie("sid", "abc", minimal_opt);
    resp.delete_cookie("sid", minimal_opt);

    const std::string serialized = resp.serialize();
    EXPECT_NE(serialized.find("Set-Cookie: sid=abc"), std::string::npos);
    EXPECT_EQ(serialized.find("Set-Cookie: sid=abc;"), std::string::npos);
    EXPECT_NE(serialized.find("Set-Cookie: sid=; Max-Age=0"), std::string::npos);
}

TEST(HttpResponseTest, SerializeWithoutBodyStillWritesHeaders) {
    HttpResponse resp;
    resp.status(HttpStatus::OK).content_type("text/plain").body("payload");

    const std::string serialized = resp.serialize(false);
    EXPECT_NE(serialized.find("Content-Length: 7"), std::string::npos);
    EXPECT_EQ(serialized.find("\r\n\r\npayload"), std::string::npos);
}

TEST(HttpResponseTest, SerializeToAcceptsNullOutputPointer) {
    HttpResponse resp;
    resp.status(HttpStatus::OK).body("ok");
    resp.serialize_to(nullptr);
    SUCCEED();
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
