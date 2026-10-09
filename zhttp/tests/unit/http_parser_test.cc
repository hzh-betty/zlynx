#include "../support/network_fixture.h"
#include "protocol/http/http_request_parser.h"
#include "znet/byte_buffer.h"

#include <gtest/gtest.h>
#include <vector>

using namespace zhttp;

class HttpRequestParserTest : public ::testing::Test {
  protected:
    void SetUp() override { parser_ = std::make_unique<HttpRequestParser>(); }

    std::unique_ptr<HttpRequestParser> parser_;
    znet::ByteBuffer buffer_;
};

TEST_F(HttpRequestParserTest, ParseSimpleGetRequest) {
    const char *request = "GET /index.html HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->state(), ParseState::COMPLETE);

    auto req = parser_->request();
    EXPECT_EQ(req->method(), HttpMethod::GET);
    EXPECT_EQ(req->path(), "/index.html");
    EXPECT_EQ(req->version(), HttpVersion::HTTP_1_1);
    EXPECT_EQ(req->header("Host"), "localhost");
    EXPECT_TRUE(req->is_keep_alive());
}

TEST_F(HttpRequestParserTest, ParsePostRequestWithBody) {
    const char *request = "POST /api/data HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: 13\r\n"
                          "\r\n"
                          "{\"key\":\"val\"}";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);

    auto req = parser_->request();
    EXPECT_EQ(req->method(), HttpMethod::POST);
    EXPECT_EQ(req->path(), "/api/data");
    EXPECT_EQ(req->content_type(), "application/json");
    EXPECT_EQ(req->content_length(), 13u);
    EXPECT_EQ(req->body(), "{\"key\":\"val\"}");
}

TEST_F(HttpRequestParserTest, ParseRequestWithQueryParams) {
    const char *request = "GET /search?q=hello&page=1 HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);

    auto req = parser_->request();
    EXPECT_EQ(req->path(), "/search");
    EXPECT_EQ(req->query(), "q=hello&page=1");
    EXPECT_EQ(req->query_param("q"), "hello");
    EXPECT_EQ(req->query_param("page"), "1");
}

TEST_F(HttpRequestParserTest, ParseIncompleteRequest) {
    const char *partial = "GET /index.html HTTP/1.1\r\n"
                          "Host: local";
    buffer_.append(partial, strlen(partial));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpRequestParserTest, ParseNullBufferReturnsError) {
    ParseResult result = parse_request(*parser_, nullptr);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->state(), ParseState::ERROR);
    EXPECT_FALSE(parser_->error().empty());
}

TEST_F(HttpRequestParserTest, ParseEmptyBufferNeedsMore) {
    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpRequestParserTest, ParseInvalidMethod) {
    const char *request = "INVALID /index.html HTTP/1.1\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->state(), ParseState::ERROR);
}

TEST_F(HttpRequestParserTest, ParseInvalidHttpVersion) {
    const char *request = "GET / HTTP/2.0\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->state(), ParseState::ERROR);
}

TEST_F(HttpRequestParserTest, ParseCompleteRequestLineWithoutHeaderTerminator) {
    const char *request = "GET /index.html HTTP/1.1\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpRequestParserTest, ParseMissingSpaceAfterMethod) {
    const char *request = "GET/index.html HTTP/1.1\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseMissingSpaceBeforeVersion) {
    const char *request = "GET /index.htmlHTTP/1.1\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseInvalidHeaderLine) {
    const char *request = "GET / HTTP/1.1\r\n"
                          "InvalidHeader\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseHeaderValueWithWhitespaceTrimmed) {
    const char *request = "GET / HTTP/1.1\r\n"
                          "Content-Type:   application/json   \r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->header("Content-Type"), "application/json");
}

TEST_F(HttpRequestParserTest, ParseVeryLongPath) {
    std::string long_path(2000, 'a');
    std::string request = "GET /" + long_path + " HTTP/1.1\r\n\r\n";
    buffer_.append(request.c_str(), request.size());

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->path(), "/" + long_path);
}

TEST_F(HttpRequestParserTest, ParseManyHeaders) {
    std::string request = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 100; ++i) {
        request += "X-Header-" + std::to_string(i) + ": value" +
                   std::to_string(i) + "\r\n";
    }
    request += "\r\n";
    buffer_.append(request.c_str(), request.size());

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->header("X-Header-50"), "value50");
}

TEST_F(HttpRequestParserTest, ParseContentLengthMismatchNeedsMore) {
    const char *request = "POST / HTTP/1.1\r\n"
                          "Content-Length: 100\r\n"
                          "\r\n"
                          "short";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::NEED_MORE);
}

TEST_F(HttpRequestParserTest, ParseIncrementalRequest) {
    const char *part1 = "GET /";
    const char *part2 = "index.html HTTP/";
    const char *part3 = "1.1\r\n";
    const char *part4 = "Host: localhost\r\n";
    const char *part5 = "\r\n";

    buffer_.append(part1, strlen(part1));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part2, strlen(part2));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part3, strlen(part3));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part4, strlen(part4));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part5, strlen(part5));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->path(), "/index.html");
    EXPECT_EQ(parser_->request()->header("Host"), "localhost");
}

TEST_F(HttpRequestParserTest, ParseAllSupportedMethods) {
    const std::vector<std::string> methods = {"GET",    "POST",    "PUT",
                                              "DELETE", "HEAD",    "OPTIONS",
                                              "PATCH",  "CONNECT", "TRACE"};

    for (const auto &method : methods) {
        parser_->reset();
        buffer_.retrieve_all();

        const std::string request =
            method + (method == "CONNECT" ? " example.com:443" : " /") +
            " HTTP/1.1\r\n\r\n";
        buffer_.append(request.c_str(), request.size());

        ParseResult result = parse_request(*parser_, &buffer_);
        EXPECT_EQ(result, ParseResult::COMPLETE) << "method=" << method;
        EXPECT_EQ(method_to_string(parser_->request()->method()), method);
    }
}

TEST_F(HttpRequestParserTest, ResetAfterComplete) {
    const char *request = "GET / HTTP/1.1\r\n\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);

    parser_->reset();
    EXPECT_EQ(parser_->state(), ParseState::REQUEST_LINE);

    // 解析新请求
    const char *request2 = "POST /api HTTP/1.1\r\n"
                           "Content-Length: 0\r\n"
                           "\r\n";
    buffer_.append(request2, strlen(request2));
    result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->method(), HttpMethod::POST);
}

TEST_F(HttpRequestParserTest, ParseChunkedPostRequest) {
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

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

TEST_F(HttpRequestParserTest, ParseChunkedWithTrailers) {
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

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "abcde");
}

TEST_F(HttpRequestParserTest, ParseChunkedWithSizeExtension) {
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

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

TEST_F(HttpRequestParserTest, RejectsTransferEncodingWithContentLength) {
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

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
    EXPECT_EQ(parser_->error_status(), HttpStatus::BAD_REQUEST);
    EXPECT_TRUE(parser_->request()->body().empty());
}

TEST_F(HttpRequestParserTest, ParseInvalidChunkSizeReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "Z\r\n"
                          "oops\r\n"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseInvalidChunkDataTerminatorReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "1\r\n"
                          "aX"
                          "0\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseInvalidChunkTrailerReturnsError) {
    const char *request = "POST /chunk HTTP/1.1\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "1\r\n"
                          "a\r\n"
                          "0\r\n"
                          "InvalidTrailer\r\n"
                          "\r\n";
    buffer_.append(request, strlen(request));

    ParseResult result = parse_request(*parser_, &buffer_);
    EXPECT_EQ(result, ParseResult::ERROR);
}

TEST_F(HttpRequestParserTest, ParseIncrementalChunkedRequest) {
    const char *part1 =
        "POST /chunk HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char *part2 = "4\r\nWiki\r\n";
    const char *part3 = "5\r\npedia\r\n";
    const char *part4 = "0\r\n\r\n";

    buffer_.append(part1, strlen(part1));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part2, strlen(part2));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part3, strlen(part3));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::NEED_MORE);

    buffer_.append(part4, strlen(part4));
    EXPECT_EQ(parse_request(*parser_, &buffer_), ParseResult::COMPLETE);
    EXPECT_EQ(parser_->request()->body(), "Wikipedia");
}

TEST(HttpRequestParserLimitsTest,
     RejectsOversizedPartialAndCompleteRequestLines) {
    HttpRequestParser::Limits limits;
    limits.max_request_line_bytes = 16;
    for (const std::string &suffix : {std::string{}, std::string("\r\n")}) {
        HttpRequestParser parser(limits);
        znet::ByteBuffer buffer;
        buffer.append(std::string("GET /") + std::string(20, 'x') + suffix);
        EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
        EXPECT_EQ(parser.error_status(), HttpStatus::URI_TOO_LONG);
    }
}

TEST(HttpRequestParserLimitsTest,
     CountsHeadersAcrossFragmentsAndResetsForNextRequest) {
    HttpRequestParser::Limits limits;
    limits.max_header_bytes = 12;
    HttpRequestParser parser(limits);
    znet::ByteBuffer buffer;
    buffer.append("GET / HTTP/1.1\r\nX: a\r\n");
    ASSERT_EQ(parse_request(parser, &buffer), ParseResult::NEED_MORE);
    buffer.append("Y: aaaa");
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
    EXPECT_EQ(parser.error_status(),
              HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE);
    buffer.retrieve_all();
    parser.reset();
    buffer.append("GET / HTTP/1.1\r\nX: abcde\r\n\r\n");
    EXPECT_EQ(parse_request(parser, &buffer),
              ParseResult::COMPLETE); // 正好 12 字节。
    parser.reset();
    buffer.append("GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n");
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::COMPLETE);
    parser.reset();
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::COMPLETE);
}

TEST(HttpRequestParserLimitsTest, EnforcesHeaderCountIncludingTrailers) {
    HttpRequestParser::Limits limits;
    limits.max_header_count = 1;
    for (const std::string &request :
         {std::string("GET / HTTP/1.1\r\nX: a\r\nY: b\r\n\r\n"),
          std::string("POST / HTTP/1.1\r\nTransfer-Encoding: "
                      "chunked\r\n\r\n0\r\nX: a\r\n\r\n")}) {
        HttpRequestParser parser(limits);
        znet::ByteBuffer buffer;
        buffer.append(request);
        EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
        EXPECT_EQ(parser.error_status(),
                  HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE);
    }
}

TEST(HttpRequestParserLimitsTest, RejectsDeclaredBodyBeforeReceivingIt) {
    HttpRequestParser::Limits limits;
    limits.max_body_bytes = 4;
    for (const char *length : {"5", "999999999999999999999999999999"}) {
        HttpRequestParser parser(limits);
        znet::ByteBuffer buffer;
        buffer.append(std::string("POST / HTTP/1.1\r\nContent-Length: ") +
                      length + "\r\n\r\n");
        EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
        EXPECT_EQ(parser.error_status(), HttpStatus::PAYLOAD_TOO_LARGE);
    }
    HttpRequestParser parser(limits);
    znet::ByteBuffer buffer;
    buffer.append("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\n1234");
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::COMPLETE);
}

TEST(HttpRequestParserLimitsTest,
     LimitsChunkedBodyCumulativelyBeforeNextChunkArrives) {
    HttpRequestParser::Limits limits;
    limits.max_body_bytes = 4;
    HttpRequestParser parser(limits);
    znet::ByteBuffer buffer;
    buffer.append(
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
    ASSERT_EQ(parse_request(parser, &buffer), ParseResult::NEED_MORE);
    buffer.append("2\r\n");
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
    EXPECT_EQ(parser.error_status(), HttpStatus::PAYLOAD_TOO_LARGE);
    buffer.retrieve_all();
    parser.reset();
    buffer.append("POST / HTTP/1.1\r\nTransfer-Encoding: "
                  "chunked\r\n\r\n2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n");
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::COMPLETE);
    EXPECT_EQ(parser.request()->body(), "abcd");
}

TEST(HttpRequestParserLimitsTest, LimitsChunkMetadataAndRejectsSizeOverflow) {
    for (const std::string &chunk :
         {std::string(1100, 'f'),
          std::string("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n")}) {
        HttpRequestParser parser;
        znet::ByteBuffer buffer;
        buffer.append("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" +
                      chunk);
        EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
    }
    HttpRequestParser::Limits limits;
    limits.max_header_bytes = 40;
    HttpRequestParser parser(limits);
    znet::ByteBuffer buffer;
    buffer.append(
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: " +
        std::string(30, 'a'));
    EXPECT_EQ(parse_request(parser, &buffer), ParseResult::ERROR);
    EXPECT_EQ(parser.error_status(),
              HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE);
}

TEST(HttpRequestParserContractTest,
     PreservesDuplicateHeadersAndSeparatesTrailers) {
    HttpRequestParser parser;
    znet::ByteBuffer input;
    input.append(
        "POST /a%2Fb?x=1&x=2 HTTP/1.1\r\nX-Value: first\r\nx-value: "
        "second\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX-Value: "
        "trailer\r\n\r\nGET /next HTTP/1.1\r\n\r\n");
    EXPECT_EQ(parser.parse(&input), ParseResult::HEADERS_READY);
    EXPECT_EQ(parser.parse(&input), ParseResult::COMPLETE);
    auto request = parser.request();
    EXPECT_EQ(request->path(), "/a%2Fb");
    EXPECT_EQ(request->query_values("x"), (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(request->headers().get_all("X-Value"),
              (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(request->trailers().get("X-Value"), "trailer");
    EXPECT_EQ(request->body(), "abc");
    parser.reset();
    EXPECT_EQ(parse_request(parser, &input), ParseResult::COMPLETE);
    EXPECT_EQ(parser.request()->path(), "/next");
    EXPECT_EQ(request->body(), "abc");
}
TEST(HttpRequestParserContractTest, ValidatesEveryLengthAndRequestTarget) {
    for (const auto &headers :
         {"Content-Length: 2\r\nContent-Length: 3\r\n",
          "Content-Length: 2,3\r\n", "Content-Length: 2,\r\n"}) {
        HttpRequestParser parser;
        znet::ByteBuffer input;
        input.append(std::string("POST / HTTP/1.1\r\n") + headers + "\r\nabc");
        EXPECT_EQ(parse_request(parser, &input), ParseResult::ERROR);
    }
    for (const auto &target :
         {"/bad%", "/bad%GG", "/path#fragment", "/bad path", "http:///empty",
          "http://", "example.com:"}) {
        HttpRequestParser parser;
        znet::ByteBuffer input;
        input.append(std::string("GET ") + target + " HTTP/1.1\r\n\r\n");
        EXPECT_EQ(parse_request(parser, &input), ParseResult::ERROR);
    }
    HttpRequestParser parser;
    znet::ByteBuffer input;
    input.append("POST / HTTP/1.1\r\nContent-Length: 2, 2\r\ncontent-length: "
                 "2\r\n\r\nab");
    EXPECT_EQ(parse_request(parser, &input), ParseResult::COMPLETE);
    EXPECT_EQ(parser.request()->body(), "ab");
}

TEST(HttpRequestParserContractTest,
     DoesNotTrimAwayInvalidFieldOrChunkControlCharacters) {
    for (const auto &wire : {"GET / HTTP/1.1\r\nX-Test: \vvalue\r\n\r\n",
                             "POST / HTTP/1.1\r\nTransfer-Encoding: "
                             "chunked\r\n\r\n1;bad=\v\r\na\r\n0\r\n\r\n",
                             "POST / HTTP/1.1\r\nTransfer-Encoding: "
                             "chunked\r\n\r\n0\r\nX-Test: value\v\r\n\r\n"}) {
        HttpRequestParser parser;
        znet::ByteBuffer input;
        input.append(wire);
        EXPECT_EQ(parse_request(parser, &input), ParseResult::ERROR);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
