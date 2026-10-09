#include "znet/error.h"
#include <gtest/gtest.h>
#include <memory>

using namespace znet;

TEST(ErrorTest, OwnsMoveOnlyValuesAndPreservesFailureContext) {
    Result<std::unique_ptr<int>> value(std::make_unique<int>(42));
    ASSERT_TRUE(value);
    auto moved = std::move(value).value();
    EXPECT_EQ(*moved, 42);
    Result<int> failure(make_error(ErrorKind::validation,
                                   std::errc::invalid_argument, "endpoint",
                                   "bad IP"));
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().kind, ErrorKind::validation);
    EXPECT_EQ(failure.error().code, std::errc::invalid_argument);
    EXPECT_NE(failure.error().message().find("bad IP"), std::string::npos);
    EXPECT_THROW(failure.value(), std::bad_variant_access);
    Result<void> success;
    EXPECT_NO_THROW(success.value());
    EXPECT_THROW(success.error(), std::logic_error);
    EXPECT_THROW((void)Result<int>(Error{}), std::invalid_argument);
}

TEST(ErrorTest, TransferRetainsProgressAlongsideError) {
    Transfer result{3, io_error("write", EPIPE), false};
    EXPECT_FALSE(result);
    EXPECT_EQ(result.bytes, 3u);
    EXPECT_EQ(result.error.kind, ErrorKind::io);
    EXPECT_EQ(result.error.code, std::errc::broken_pipe);
}
