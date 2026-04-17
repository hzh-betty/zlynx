#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/context.h"

namespace zco {
namespace {

class ContextUnitTest : public test::RuntimeTestBase {};

TEST_F(ContextUnitTest, SwapContextRejectsNullInputs) {
    Context context;
    EXPECT_EQ(Context::swap_context(nullptr, &context), -1);
    EXPECT_EQ(Context::swap_context(&context, nullptr), -1);
}

TEST_F(ContextUnitTest, GetContextReturnsSuccessOnCurrentThread) {
    Context context;
    EXPECT_EQ(context.get_context(), 0);
}

} // namespace
} // namespace zco
