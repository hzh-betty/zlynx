#include "zhttp/http_common.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

TEST(HttpCommonTest, MethodStringConversionsCoverKnownAndUnknownValues) {
    EXPECT_STREQ(method_to_string(HttpMethod::GET), "GET");
    EXPECT_STREQ(method_to_string(HttpMethod::POST), "POST");
    EXPECT_STREQ(method_to_string(HttpMethod::PUT), "PUT");
    EXPECT_STREQ(method_to_string(HttpMethod::DELETE), "DELETE");
    EXPECT_STREQ(method_to_string(HttpMethod::HEAD), "HEAD");
    EXPECT_STREQ(method_to_string(HttpMethod::OPTIONS), "OPTIONS");
    EXPECT_STREQ(method_to_string(HttpMethod::PATCH), "PATCH");
    EXPECT_STREQ(method_to_string(HttpMethod::CONNECT), "CONNECT");
    EXPECT_STREQ(method_to_string(HttpMethod::TRACE), "TRACE");
    EXPECT_STREQ(method_to_string(static_cast<HttpMethod>(-1)), "UNKNOWN");

    EXPECT_EQ(string_to_method("get"), HttpMethod::GET);
    EXPECT_EQ(string_to_method("POST"), HttpMethod::POST);
    EXPECT_EQ(string_to_method("PuT"), HttpMethod::PUT);
    EXPECT_EQ(string_to_method("delete"), HttpMethod::DELETE);
    EXPECT_EQ(string_to_method("head"), HttpMethod::HEAD);
    EXPECT_EQ(string_to_method("options"), HttpMethod::OPTIONS);
    EXPECT_EQ(string_to_method("patch"), HttpMethod::PATCH);
    EXPECT_EQ(string_to_method("connect"), HttpMethod::CONNECT);
    EXPECT_EQ(string_to_method("trace"), HttpMethod::TRACE);
    EXPECT_EQ(string_to_method("unknown"), HttpMethod::UNKNOWN);
}

TEST(HttpCommonTest, StatusAndVersionStringConversions) {
    EXPECT_STREQ(status_to_string(HttpStatus::CONTINUE), "Continue");
    EXPECT_STREQ(status_to_string(HttpStatus::OK), "OK");
    EXPECT_STREQ(status_to_string(HttpStatus::PARTIAL_CONTENT),
                 "Partial Content");
    EXPECT_STREQ(status_to_string(HttpStatus::MOVED_PERMANENTLY),
                 "Moved Permanently");
    EXPECT_STREQ(status_to_string(HttpStatus::BAD_REQUEST), "Bad Request");
    EXPECT_STREQ(status_to_string(HttpStatus::INTERNAL_SERVER_ERROR),
                 "Internal Server Error");
    EXPECT_STREQ(status_to_string(static_cast<HttpStatus>(9999)), "Unknown");

    EXPECT_STREQ(version_to_string(HttpVersion::HTTP_1_0), "HTTP/1.0");
    EXPECT_STREQ(version_to_string(HttpVersion::HTTP_1_1), "HTTP/1.1");
    EXPECT_STREQ(version_to_string(HttpVersion::UNKNOWN), "HTTP/1.1");

    EXPECT_EQ(string_to_version("HTTP/1.0"), HttpVersion::HTTP_1_0);
    EXPECT_EQ(string_to_version("HTTP/1.1"), HttpVersion::HTTP_1_1);
    EXPECT_EQ(string_to_version("HTTP/2"), HttpVersion::UNKNOWN);
}

TEST(HttpCommonTest, MimeAndStringUtilitiesHandleEdgeCases) {
    EXPECT_STREQ(get_mime_type("HTML"), "text/html");
    EXPECT_STREQ(get_mime_type("woff2"), "font/woff2");
    EXPECT_STREQ(get_mime_type(""), "application/octet-stream");
    EXPECT_STREQ(get_mime_type("unknown_ext"), "application/octet-stream");

    EXPECT_EQ(to_lower("HeLLo123"), "hello123");

    std::string spaced = " \t value with spaces \n";
    trim(spaced);
    EXPECT_EQ(spaced, "value with spaces");

    std::vector<std::string> parts = split_string("a,,b,", ',');
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "");
    EXPECT_EQ(parts[2], "b");
    EXPECT_EQ(parts[3], "");

    EXPECT_EQ(join_string(parts, "|"), "a||b|");
    EXPECT_EQ(join_string({}, ","), "");
}

TEST(HttpCommonTest, UrlDecodeHandlesValidAndInvalidEscapes) {
    EXPECT_EQ(url_decode("hello%20world"), "hello world");
    EXPECT_EQ(url_decode("a+b+c"), "a b c");
    EXPECT_EQ(url_decode("%2fpath%2Fto%2Ffile"), "/path/to/file");
    EXPECT_EQ(url_decode("%zz%4"), "%zz%4");
    EXPECT_EQ(url_decode("%"), "%");
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
