#include "zhttp/internal/http_parser.h"
#include "zhttp/zhttp_logger.h"
#include "znet/buffer.h"

#include <gtest/gtest.h>
#include <vector>

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

TEST_F(HttpParserTest, ParseNullBufferReturnsError) {
    ParseResult result = parser_->parse(nullptr);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->state(), ParseState::ERROR);
    EXPECT_FALSE(parser_->error().empty());
}

TEST_F(HttpParserTest, ParseEmptyBufferNeedsMore) {
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

TEST_F(HttpParserTest, ParseInvalidHttpVersion) {
    const char *request = "GET / HTTP/2.0\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->state(), ParseState::ERROR);
}

TEST_F(HttpParserTest, ParseCompleteRequestLineWithoutHeaderTerminator) {
    const char *request = "GET /index.html HTTP/1.1\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserTest, ParseMissingSpaceAfterMethod) {
    const char *request = "GET/index.html HTTP/1.1\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseMissingSpaceBeforeVersion) {
    const char *request = "GET /index.htmlHTTP/1.1\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseInvalidHeaderLine) {
    const char *request = "GET / HTTP/1.1\r\n"
                          "InvalidHeader\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseHeaderValueWithWhitespaceTrimmed) {
    const char *request = "GET / HTTP/1.1\r\n"
                          "Content-Type:   application/json   \r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->header("Content-Type"), "application/json");
}

TEST_F(HttpParserTest, ParseVeryLongPath) {
    std::string long_path(2000, 'a');
    std::string request = "GET /" + long_path + " HTTP/1.1\r\n\r\n";
    buffer_.append(request.c_str(), request.size());

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->path(), "/" + long_path);
}

TEST_F(HttpParserTest, ParseManyHeaders) {
    std::string request = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 100; ++i) {
        request += "X-Header-" + std::to_string(i) + ": value" +
                   std::to_string(i) + "\r\n";
    }
    request += "\r\n";
    buffer_.append(request.c_str(), request.size());

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->header("X-Header-50"), "value50");
}

TEST_F(HttpParserTest, ParseContentLengthMismatchNeedsMore) {
    const char *request = "POST / HTTP/1.1\r\n"
                          "Content-Length: 100\r\n"
                          "\r\n"
                          "short";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpParserTest, ParseIncrementalRequest) {
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

TEST_F(HttpParserTest, ParseAllSupportedMethods) {
    const std::vector<std::string> methods = {"GET",    "POST",    "PUT",
                                              "DELETE", "HEAD",    "OPTIONS",
                                              "PATCH",  "CONNECT", "TRACE"};

    for (const auto &method : methods) {
        parser_->reset();
        buffer_.retrieve_all();

        const std::string request = method + " / HTTP/1.1\r\n\r\n";
        buffer_.append(request.c_str(), request.size());

        ParseResult result = parser_->parse(&buffer_);
        EXPECT_EQ(result, ParseResult::COMPLETE) << "method=" << method;
        EXPECT_EQ(method_to_string(parser_->request()->method()), method);
    }
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

TEST_F(HttpParserTest, ParseChunkedPostRequest) {
    const char *request = "POST /api/chunk HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4\r\n"
                          "Wiki\r\n"
                          "5\r\n"
                          "pedia\r\n"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

TEST_F(HttpParserTest, ParseChunkedWithTrailers) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "3\r\n"
                          "abc\r\n"
                          "2\r\n"
                          "de\r\n"
                          "0\r\n"
                          "X-Trace: yes\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "abcde");
}

TEST_F(HttpParserTest, ParseChunkedWithSizeExtension) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4;ext=1\r\n"
                          "Wiki\r\n"
                          "5\r\n"
                          "pedia\r\n"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

TEST_F(HttpParserTest, ChunkedTransferEncodingTakesPriorityOverContentLength) {
    const char *request = "POST /api/chunk HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: 999\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4\r\n"
                          "test\r\n"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "test");
}

TEST_F(HttpParserTest, ParseInvalidChunkSizeReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "Z\r\n"
                          "oops\r\n"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseInvalidChunkDataTerminatorReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "1\r\n"
                          "aX"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseInvalidChunkTrailerReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "1\r\n"
                          "a\r\n"
                          "0\r\n"
                          "InvalidTrailer\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parser_->parse(&buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpParserTest, ParseIncrementalChunkedRequest) {
    const char *part1 =
        "POST /chunk HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char *part2 = "4\r\nWiki\r\n";
    const char *part3 = "5\r\npedia\r\n";
    const char *part4 = "0\r\n\r\n";

    buffer_.append(part1, strlen(part1));
    EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

    buffer_.append(part2, strlen(part2));
    EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

    buffer_.append(part3, strlen(part3));
    EXPECT_EQ(parser_->parse(&buffer_), ParseResult::NEED_MORE);

    buffer_.append(part4, strlen(part4));
    EXPECT_EQ(parser_->parse(&buffer_), ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
