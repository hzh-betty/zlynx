#include "buff.h"
#include "http_parser.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

class HttpParserDetailedTest : public ::testing::Test {
protected:
  void SetUp() override { parser_ = std::make_unique<HttpParser>(); }

  std::unique_ptr<HttpParser> parser_;
  znet::Buffer buffer_;
};

// ========== 边界测试 ==========

TEST_F(HttpParserDetailedTest, EmptyBuffer) {
  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserDetailedTest, OnlyMethod) {
  const char *request = "GET";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserDetailedTest, MethodAndPath) {
  const char *request = "GET /index.html";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserDetailedTest, CompleteRequestLineNoHeaders) {
  const char *request = "GET /index.html HTTP/1.1\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE); // 需要至少一个空行
}

TEST_F(HttpParserDetailedTest, VeryLongPath) {
  std::string long_path(2000, 'a');
  std::string request = "GET /" + long_path + " HTTP/1.1\r\n\r\n";
  buffer_.append(request.c_str(), request.size());

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->path(), "/" + long_path);
}

TEST_F(HttpParserDetailedTest, ManyHeaders) {
  std::string request = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 100; ++i) {
    request += "X-Header-" + std::to_string(i) + ": value" + std::to_string(i) +
               "\r\n";
  }
  request += "\r\n";
  buffer_.append(request.c_str(), request.size());

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->header("X-Header-50"), "value50");
}

TEST_F(HttpParserDetailedTest, LargeBody) {
  std::string body(10000, 'x');
  std::string request = "POST /data HTTP/1.1\r\n"
                        "Content-Length: 10000\r\n"
                        "\r\n" +
                        body;
  buffer_.append(request.c_str(), request.size());

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->body().size(), 10000u);
}

// ========== 错误处理测试 ==========

TEST_F(HttpParserDetailedTest, InvalidHttpVersion) {
  const char *request = "GET / HTTP/2.0\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::ERROR);
  EXPECT_FALSE(parser_->error().empty());
}

TEST_F(HttpParserDetailedTest, MissingSpaceAfterMethod) {
  const char *request = "GET/index.html HTTP/1.1\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserDetailedTest, MissingSpaceBeforeVersion) {
  const char *request = "GET /index.htmlHTTP/1.1\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserDetailedTest, InvalidHeaderFormat) {
  const char *request = "GET / HTTP/1.1\r\n"
                        "InvalidHeader\r\n"
                        "\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserDetailedTest, ContentLengthMismatch) {
  const char *request = "POST / HTTP/1.1\r\n"
                        "Content-Length: 100\r\n"
                        "\r\n"
                        "short";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::NEED_MORE); // 需要更多数据
}

// ========== 特殊字符测试 ==========

TEST_F(HttpParserDetailedTest, PathWithSpecialChars) {
  const char *request =
      "GET /path%20with%20spaces?key=value%26more HTTP/1.1\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->path(), "/path%20with%20spaces");
}

TEST_F(HttpParserDetailedTest, HeaderWithWhitespace) {
  const char *request = "GET / HTTP/1.1\r\n"
                        "Content-Type:   application/json   \r\n"
                        "\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->header("Content-Type"), "application/json");
}

TEST_F(HttpParserDetailedTest, MultipleQueryParams) {
  const char *request =
      "GET /search?q=test&page=1&sort=desc&filter=active HTTP/1.1\r\n\r\n";
  buffer_.append(request, strlen(request));

  ParseResult result = parser_->parse(&buffer_);
  EXPECT_EQ(result, ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->query_param("q"), "test");
  EXPECT_EQ(parser_->request()->query_param("page"), "1");
  EXPECT_EQ(parser_->request()->query_param("sort"), "desc");
  EXPECT_EQ(parser_->request()->query_param("filter"), "active");
}

// ========== 流式解析测试 ==========

TEST_F(HttpParserDetailedTest, IncrementalParsing) {
  const char *part1 = "GET /";
  const char *part2 = "index.html HTTP/";
  const char *part3 = "1.1\r\n";
  const char *part4 = "Host: localhost\r\n";
  const char *part5 = "\r\n";

  buffer_.append(part1, strlen(part1));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

  buffer_.append(part2, strlen(part2));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

  buffer_.append(part3, strlen(part3));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

  buffer_.append(part4, strlen(part4));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

  buffer_.append(part5, strlen(part5));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::COMPLETE);

  EXPECT_EQ(parser_->request()->path(), "/index.html");
  EXPECT_EQ(parser_->request()->header("Host"), "localhost");
}

// ========== HTTP方法测试 ==========

TEST_F(HttpParserDetailedTest, AllHttpMethods) {
  std::vector<std::string> methods = {"GET",    "POST",    "PUT",
                                      "DELETE", "HEAD",    "OPTIONS",
                                      "PATCH",  "CONNECT", "TRACE"};

  for (const auto &method : methods) {
    parser_->reset();
    buffer_.retrieve_all();

    std::string request = method + " / HTTP/1.1\r\n\r\n";
    buffer_.append(request.c_str(), request.size());

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE) << "Failed for method: " << method;
    EXPECT_EQ(method_to_string(parser_->request()->method()), method);
  }
}

// ========== Keep-Alive测试 ==========

TEST_F(HttpParserDetailedTest, MultipleRequestsKeepAlive) {
  const char *request1 =
      "GET /first HTTP/1.1\r\nConnection: keep-alive\r\n\r\n";
  const char *request2 =
      "POST /second HTTP/1.1\r\nContent-Length: 4\r\n\r\ntest";

  buffer_.append(request1, strlen(request1));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->path(), "/first");
  EXPECT_TRUE(parser_->request()->is_keep_alive());

  parser_->reset();
  buffer_.append(request2, strlen(request2));
  EXPECT_EQ(parser_->parse(&buffer_), ParseResult::COMPLETE);
  EXPECT_EQ(parser_->request()->path(), "/second");
  EXPECT_EQ(parser_->request()->body(), "test");
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
