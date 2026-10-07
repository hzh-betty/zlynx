/**
 * @file context_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/context.h"

namespace zco {
namespace {

class ContextUnitTest : public test::RuntimeTestBase {};

void NoopContextEntry() {}

TEST_F(ContextUnitTest, SwapContextRejectsNullInputs) {
    Context context;
    EXPECT_EQ(Context::swap_context(nullptr, &context), -1);
    EXPECT_EQ(Context::swap_context(&context, nullptr), -1);
}

TEST_F(ContextUnitTest, MakeContextInitializesStackAndLink) {
    char stack[16 * 1024];
    Context context;
    Context link;
    context.make_context(stack, sizeof(stack), &NoopContextEntry, link.get());

    EXPECT_EQ(context.get()->uc_stack.ss_sp, stack);
    EXPECT_EQ(context.get()->uc_stack.ss_size, sizeof(stack));
    EXPECT_EQ(context.get()->uc_link, link.get());
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
