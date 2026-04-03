#include "http_response.h"
#include "zhttp_logger.h"

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
  resp.status(HttpStatus::OK).content_type("text/plain").body("Hello").enable_chunked();

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
      [&called](HttpResponse::AsyncChunkSender, HttpResponse::AsyncStreamCloser) {
        called = true;
      });

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

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
