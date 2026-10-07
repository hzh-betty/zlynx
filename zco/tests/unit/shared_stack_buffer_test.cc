/**
 * @file shared_stack_buffer_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/fiber_stack_manager.h"
#include "zco/internal/shared_stack_buffer.h"

namespace zco {
namespace {

class SharedStackBufferUnitTest : public test::RuntimeTestBase {};

TEST_F(SharedStackBufferUnitTest, ZeroSizedBufferHasNullPointers) {
    SharedStackBuffer buffer(0);

    EXPECT_EQ(buffer.data(), nullptr);
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.occupy_fiber().fiber, nullptr);
    EXPECT_EQ(buffer.occupy_fiber().fiber_id, 0);
}

TEST_F(SharedStackBufferUnitTest, MoveConstructorTransfersOwnership) {
    SharedStackBuffer original(1024);
    ASSERT_NE(original.data(), nullptr);

    char *original_data = original.data();

    SharedStackBuffer moved(std::move(original));
    EXPECT_EQ(moved.data(), original_data);
    EXPECT_EQ(moved.size(), 1024u);

    EXPECT_EQ(original.data(), nullptr);
    EXPECT_EQ(original.size(), 0u);
}

TEST_F(SharedStackBufferUnitTest, MoveAssignmentTransfersOwnership) {
    SharedStackBuffer left(512);
    SharedStackBuffer right(2048);

    char *right_data = right.data();

    left = std::move(right);
    EXPECT_EQ(left.data(), right_data);
    EXPECT_EQ(left.size(), 2048u);

    EXPECT_EQ(right.data(), nullptr);
    EXPECT_EQ(right.size(), 0u);
}

TEST_F(SharedStackBufferUnitTest, MoveAssignmentSelfIsNoOp) {
    SharedStackBuffer buffer(256);
    char *data_before = buffer.data();
    const size_t size_before = buffer.size();

    buffer = std::move(buffer);

    EXPECT_EQ(buffer.data(), data_before);
    EXPECT_EQ(buffer.size(), size_before);
}

TEST_F(SharedStackBufferUnitTest, OccupyFiberSetterAndGetterWork) {
    SharedStackBuffer buffer(256);

    Fiber *sentinel = reinterpret_cast<Fiber *>(0x1);
    buffer.set_occupy_fiber(sentinel, 7);
    EXPECT_EQ(buffer.occupy_fiber().fiber, sentinel);
    EXPECT_EQ(buffer.occupy_fiber().fiber_id, 7);

    buffer.set_occupy_fiber(nullptr, 99);
    EXPECT_EQ(buffer.occupy_fiber().fiber, nullptr);
    EXPECT_EQ(buffer.occupy_fiber().fiber_id, 0);
}

TEST_F(SharedStackBufferUnitTest, StackManagerAccessAndBounds) {
    FiberStackManager stacks(0, 4, 1024);

    EXPECT_EQ(stacks.count(), 4u);
    EXPECT_NE(stacks.data(0), nullptr);
    EXPECT_EQ(stacks.size(0), 1024u);

    EXPECT_EQ(stacks.data(9), nullptr);
    EXPECT_EQ(stacks.size(9), 0u);
}

TEST_F(SharedStackBufferUnitTest, StackManagerSlotsWrapWithoutReallocating) {
    FiberStackManager stacks(0, 2, 1024);
    void *first = stacks.data(0);
    void *second = stacks.data(1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);

    EXPECT_EQ(stacks.next_slot(), 0u);
    EXPECT_EQ(stacks.next_slot(), 1u);
    EXPECT_EQ(stacks.next_slot(), 0u);
    EXPECT_EQ(stacks.data(0), first);
    EXPECT_EQ(stacks.data(1), second);
}

TEST_F(SharedStackBufferUnitTest, StackManagerWithNoSlotsIsSafe) {
    FiberStackManager stacks(0, 0, 1024);
    EXPECT_EQ(stacks.count(), 0u);
    EXPECT_EQ(stacks.next_slot(), 0u);
    EXPECT_EQ(stacks.data(0), nullptr);
    EXPECT_EQ(stacks.size(0), 0u);
}

TEST_F(SharedStackBufferUnitTest, ConstAccessorsExposeSamePointers) {
    SharedStackBuffer buffer(128);
    const SharedStackBuffer &const_ref = buffer;

    EXPECT_EQ(const_ref.data(), buffer.data());
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
