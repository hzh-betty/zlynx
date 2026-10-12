#include "../support/request_builder.h"
#include "protocol/http/response_encoder.h"
#include "static_files/range.h"

#include <cerrno>
#include <gtest/gtest.h>
#include <limits>

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

TEST(RangeParseTest, RejectsPartialNumbersSignsAndOverflow) {
    auto request = make_request();
    const std::string overflow =
        std::to_string(std::numeric_limits<unsigned long long>::max()) + "0";
    for (const std::string &token :
         {std::string("+1"), std::string("1x"), std::string("0x1"),
          std::string("1 2"), std::string("1\x80", 2), overflow}) {
        SCOPED_TRACE(token);
        request->set_header("Range", "bytes=" + token + "-5");
        EXPECT_EQ(parse_range_request(*request, 10, "").state,
                  RangeParseState::INVALID);
        request->set_header("Range", "bytes=0-" + token);
        EXPECT_EQ(parse_range_request(*request, 10, "").state,
                  RangeParseState::INVALID);
        request->set_header("Range", "bytes=-" + token);
        EXPECT_EQ(parse_range_request(*request, 10, "").state,
                  RangeParseState::NOT_SATISFIABLE);
    }
}

TEST(RangeParseTest, PreservesLeadingZerosWhitespaceAndMaximumInteger) {
    auto request = make_request();
    request->set_header("Range", "bytes= 0002 - 0005 ");
    const auto parsed = parse_range_request(*request, 10, "");
    ASSERT_EQ(parsed.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(parsed.start, 2u);
    EXPECT_EQ(parsed.end, 5u);

    const auto maximum = std::numeric_limits<unsigned long long>::max();
    request->set_header("Range", "bytes=0-" + std::to_string(maximum));
    const auto maximum_end = parse_range_request(*request, 10, "");
    ASSERT_EQ(maximum_end.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(maximum_end.end, 9u);
    request->set_header("Range", "bytes=-" + std::to_string(maximum));
    const auto maximum_suffix = parse_range_request(*request, 10, "");
    ASSERT_EQ(maximum_suffix.state, RangeParseState::SATISFIABLE);
    EXPECT_EQ(maximum_suffix.start, 0u);
}

TEST(RangeParseTest, PreservesExistingErrnoBehavior) {
    struct Case {
        std::string token;
        RangeParseState state;
        int error;
    };

    const std::string overflow =
        std::to_string(std::numeric_limits<unsigned long long>::max()) + "0";
    auto request = make_request();
    for (const auto &entry :
         {Case{"2", RangeParseState::SATISFIABLE, EDOM},
          Case{"1x", RangeParseState::INVALID, EDOM},
          Case{overflow, RangeParseState::INVALID, ERANGE},
          Case{overflow + "x", RangeParseState::INVALID, EDOM}}) {
        SCOPED_TRACE(entry.token);
        request->set_header("Range", "bytes=" + entry.token + "-5");
        errno = EDOM;
        const auto parsed = parse_range_request(*request, 10, "");
        const int error = errno;
        EXPECT_EQ(parsed.state, entry.state);
        EXPECT_EQ(error, entry.error);
    }
}

} // namespace
} // namespace zhttp
