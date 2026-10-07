/**
 * @file shared_stack_buffer_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include <gtest/gtest.h>

#include "support/test_fixture.h"
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

TEST_F(SharedStackBufferUnitTest, SharedStackPoolAccessAndBounds) {
    SharedStackPool pool(4, 1024);

    EXPECT_EQ(pool.count(), 4u);
    EXPECT_NE(pool.data(0), nullptr);
    EXPECT_EQ(pool.size(0), 1024u);

    EXPECT_EQ(pool.data(9), nullptr);
    EXPECT_EQ(pool.size(9), 0u);
}

TEST_F(SharedStackBufferUnitTest, SharedStackPoolOwnerTracksFiberAndId) {
    SharedStackPool pool(2, 1024);

    Fiber *sentinel = reinterpret_cast<Fiber *>(0x1);
    pool.set_occupy_fiber(1, sentinel, 42);

    SharedStackOwner owner = pool.occupy_fiber(1);
    EXPECT_EQ(owner.fiber, sentinel);
    EXPECT_EQ(owner.fiber_id, 42);

    pool.set_occupy_fiber(1, nullptr, 42);
    owner = pool.occupy_fiber(1);
    EXPECT_EQ(owner.fiber, nullptr);
    EXPECT_EQ(owner.fiber_id, 0);

    owner = pool.occupy_fiber(9);
    EXPECT_EQ(owner.fiber, nullptr);
    EXPECT_EQ(owner.fiber_id, 0);
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
