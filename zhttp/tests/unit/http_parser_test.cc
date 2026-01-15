#include "buff.h"
#include "http_parser.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

class HttpParserTest : public ::testing::Test {
protected:
  void SetUp() override { parser_ = std::make_unique<HttpParser>(); }

  std::unique_ptr<HttpParser> parser_;
  znet::Buffer buffer_;
};

TEST_F(HttpParserTest, ParseSimpleGetRequest) {
  const char *request = "GET /index.html HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->state(), ParseState::COMPLETE);

  auto req = parser_->request();
  EXPECT_EQ(req->method(), HttpMethod::GET);
  EXPECT_EQ(req->path(), "/index.html");
  EXPECT_EQ(req->version(), HttpVersion::HTTP_1_1);
  EXPECT_EQ(req->header("Host"), "localhost");
  EXPECT_TRUE(req->is_keep_alive());
}

TEST_F(HttpParserTest, ParsePostRequestWithBody) {
  const char *request = "POST /api/data HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 13\r\n"
                        "\r\n"
                        "{\"key\":\"val\"}";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);

  auto req = parser_->request();
  EXPECT_EQ(req->method(), HttpMethod::POST);
  EXPECT_EQ(req->path(), "/api/data");
  EXPECT_EQ(req->content_type(), "application/json");
  EXPECT_EQ(req->content_length(), 13u);
  EXPECT_EQ(req->body(), "{\"key\":\"val\"}");
}

TEST_F(HttpParserTest, ParseRequestWithQueryParams) {
  const char *request = "GET /search?q=hello&page=1 HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);

  auto req = parser_->request();
  EXPECT_EQ(req->path(), "/search");
  EXPECT_EQ(req->query(), "q=hello&page=1");
  EXPECT_EQ(req->query_param("q"), "hello");
  EXPECT_EQ(req->query_param("page"), "1");
}

TEST_F(HttpParserTest, ParseIncompleteRequest) {
  const char *partial = "GET /index.html HTTP/1.1\r\n"
                        "Host: local";
  buffer_.append(partial, strlen(partial));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserTest, ParseInvalidMethod) {
  const char *request = "INVALID /index.html HTTP/1.1\r\n"
                        "\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::ERROR);
  EXPECT_EQ(parser_->state(), ParseState::ERROR);
}

TEST_F(HttpParserTest, ResetAfterComplete) {
  const char *request = "GET / HTTP/1.1\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);

  parser_->reset();
  EXPECT_EQ(parser_->state(), ParseState::REQUEST_LINE);

  // 解析新请求
  const char *request2 = "POST /api HTTP/1.1\r\n"
                         "Content-Length: 0\r\n"
                         "\r\n";
  buffer_.append(request2, strlen(request2));
  result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->method(), HttpMethod::POST);
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
