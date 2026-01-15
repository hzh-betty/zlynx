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

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
