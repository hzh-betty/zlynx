#include "protocol/http/response_encoder.h"
#include "../support/request_builder.h"
#include "static_files/range.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

TestContext::ptr make_request(HttpMethod method = HttpMethod::GET) {
    auto request = std::make_shared<TestContext>();
    request->set_method(method);
    request->set_version(HttpVersion::HTTP_1_1);
    return request;
}

TEST(RangeParseTest, ParsesSatisfiableSingleRangeForms) {
    auto request = make_request();
    request->set_header("Range", "bytes=2-5");
    ParsedRange parsed = parse_range_request(*request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 2u);
    EXPECT_EQ(parsed.end, 5u);

    request->set_header("Range", "bytes=7-");
    parsed = parse_range_request(*request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 7u);
    EXPECT_EQ(parsed.end, 9u);

    request->set_header("Range", "bytes=-3");
    parsed = parse_range_request(*request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 7u);
    EXPECT_EQ(parsed.end, 9u);

    request->set_header("Range", "bytes=3-999");
    parsed = parse_range_request(*request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 3u);
    EXPECT_EQ(parsed.end, 9u);
}

TEST(RangeParseTest, HandlesIfRangeFallbackAndNoRange) {
    auto request = make_request();
    ParsedRange parsed = parse_range_request(*request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    request->set_header("Range", "bytes=1-2");
    request->set_header("If-Range", "xyz");
    parsed = parse_range_request(*request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    parsed = parse_range_request(*request, 10, "");
    EXPECT_EQ(parsed.state, RangeParseState::NONE);

    request->set_header("If-Range", "abc");
    parsed = parse_range_request(*request, 10, "abc");
    EXPECT_EQ(parsed.state, RangeParseState::SATISFIABLE);
}

TEST(RangeParseTest, RejectsInvalidRangeGrammar) {
    auto request = make_request();
    request->set_header("Range", "items=1-2");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=1-2,4-5");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=abc-5");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=3-xyz");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);

    request->set_header("Range", "bytes=-");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::INVALID);
}

TEST(RangeParseTest, ReturnsNotSatisfiableForSemanticErrors) {
    auto request = make_request();
    request->set_header("Range", "bytes=100-200");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=8-3");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=-0");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=-abc");
    EXPECT_EQ(parse_range_request(*request, 10, "").state,
              RangeParseState::NOT_SATISFIABLE);

    request->set_header("Range", "bytes=0-1");
    EXPECT_EQ(parse_range_request(*request, 0, "").state,
              RangeParseState::NOT_SATISFIABLE);
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
