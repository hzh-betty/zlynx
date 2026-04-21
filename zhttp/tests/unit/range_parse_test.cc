#include "zhttp/internal/range_parse.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

HttpRequest::ptr make_request(HttpMethod method = HttpMethod::GET) {
    auto request = std::make_shared<HttpRequest>();
    request->set_method(method);
    request->set_version(HttpVersion::HTTP_1_1);
    return request;
}

TEST(RangeParseTest, ParsesSatisfiableSingleRangeForms) {
    auto request = make_request();
    request->set_header("Range", "bytes=2-5");
    ParsedRange parsed = parse_range_request(request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 2u);
    EXPECT_EQ(parsed.end, 5u);

    request->set_header("Range", "bytes=7-");
    parsed = parse_range_request(request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 7u);
    EXPECT_EQ(parsed.end, 9u);

    request->set_header("Range", "bytes=-3");
    parsed = parse_range_request(request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 7u);
    EXPECT_EQ(parsed.end, 9u);

    request->set_header("Range", "bytes=3-999");
    parsed = parse_range_request(request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 3u);
    EXPECT_EQ(parsed.end, 9u);
}

TEST(RangeParseTest, HandlesIfRangeFallbackAndNoRange) {
    auto request = make_request();
    ParsedRange parsed = parse_range_request(request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    request->set_header("Range", "bytes=1-2");
    request->set_header("If-Range", "xyz");
    parsed = parse_range_request(request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    parsed = parse_range_request(request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    request->set_header("If-Range", "abc");
    parsed = parse_range_request(request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
}

TEST(RangeParseTest, RejectsInvalidRangeGrammar) {
    auto request = make_request();
    request->set_header("Range", "items=1-2");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=1-2,4-5");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=abc-5");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=3-xyz");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=-");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::INVALID);
}

TEST(RangeParseTest, ReturnsNotSatisfiableForSemanticErrors) {
    auto request = make_request();
    request->set_header("Range", "bytes=100-200");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=8-3");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=-0");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=-abc");
    EXPECT_EQ(parse_range_request(request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=0-1");
    EXPECT_EQ(parse_range_request(request, 0, "").state,
              RangeParseState::NOT_SATISFIABLE);
}

TEST(RangeParseTest, WritePayloadByRangeHandlesNotSatisfiable) {
    auto request = make_request();
    HttpResponse response;
    ParsedRange parsed;
    parsed.state = RangeParseState::NOT_SATISFIABLE;

    write_payload_by_range(request, response, parsed, 10, "0123456789");

    EXPECT_EQ(response.status_code(),
              HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE);
    EXPECT_EQ(response.headers().at("Content-Range"), "bytes */10");
    EXPECT_TRUE(response.body_content().empty());
}

TEST(RangeParseTest, WritePayloadByRangeHandlesSatisfiableGetAndHead) {
    ParsedRange parsed;
    parsed.state = RangeParseState::SATISFIABLE;
    parsed.start = 2;
    parsed.end = 5;

    auto get_req = make_request(HttpMethod::GET);
    HttpResponse get_resp;
    write_payload_by_range(get_req, get_resp, parsed, 10, "0123456789");
    EXPECT_EQ(get_resp.status_code(), HttpStatus::PARTIAL_CONTENT);
    EXPECT_EQ(get_resp.headers().at("Content-Range"), "bytes 2-5/10");
    EXPECT_EQ(get_resp.headers().at("Content-Length"), "4");
    EXPECT_EQ(get_resp.body_content(), "2345");

    auto head_req = make_request(HttpMethod::HEAD);
    HttpResponse head_resp;
    write_payload_by_range(head_req, head_resp, parsed, 10, "0123456789");
    EXPECT_EQ(head_resp.status_code(), HttpStatus::PARTIAL_CONTENT);
    EXPECT_EQ(head_resp.headers().at("Content-Range"), "bytes 2-5/10");
    EXPECT_EQ(head_resp.headers().at("Content-Length"), "4");
    EXPECT_TRUE(head_resp.body_content().empty());
}

TEST(RangeParseTest, WritePayloadByRangeFallsBackToFullEntity) {
    auto get_req = make_request(HttpMethod::GET);
    auto head_req = make_request(HttpMethod::HEAD);
    ParsedRange invalid;
    invalid.state = RangeParseState::INVALID;
    ParsedRange none;
    none.state = RangeParseState::NONE;

    HttpResponse get_resp;
    write_payload_by_range(get_req, get_resp, invalid, 10, "0123456789");
    EXPECT_EQ(get_resp.status_code(), HttpStatus::OK);
    EXPECT_EQ(get_resp.body_content(), "0123456789");

    HttpResponse head_resp;
    write_payload_by_range(head_req, head_resp, none, 10, "0123456789");
    EXPECT_EQ(head_resp.status_code(), HttpStatus::OK);
    EXPECT_EQ(head_resp.headers().at("Content-Length"), "10");
    EXPECT_TRUE(head_resp.body_content().empty());
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
