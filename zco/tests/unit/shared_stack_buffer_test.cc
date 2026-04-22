#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/shared_stack_buffer.h"

namespace zco {
namespace {

class SharedStackBufferUnitTest : public test::RuntimeTestBase {};

TEST_F(SharedStackBufferUnitTest, ZeroSizedBufferHasNullPointers) {
    SharedStackBuffer buffer(0);

    EXPECT_EQ(buffer.data(), nullptr);
    EXPECT_EQ(buffer.stack_bp(), nullptr);
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.occupy_fiber(), nullptr);
}

TEST_F(SharedStackBufferUnitTest, MoveConstructorTransfersOwnership) {
    SharedStackBuffer original(1024);
    ASSERT_NE(original.data(), nullptr);

    char *original_data = original.data();
    char *original_bp = original.stack_bp();

    SharedStackBuffer moved(std::move(original));
    EXPECT_EQ(moved.data(), original_data);
    EXPECT_EQ(moved.stack_bp(), original_bp);
    EXPECT_EQ(moved.size(), 1024u);

    EXPECT_EQ(original.data(), nullptr);
    EXPECT_EQ(original.stack_bp(), nullptr);
    EXPECT_EQ(original.size(), 0u);
}

TEST_F(SharedStackBufferUnitTest, MoveAssignmentTransfersOwnership) {
    SharedStackBuffer left(512);
    SharedStackBuffer right(2048);

    char *right_data = right.data();
    char *right_bp = right.stack_bp();

    left = std::move(right);
    EXPECT_EQ(left.data(), right_data);
    EXPECT_EQ(left.stack_bp(), right_bp);
    EXPECT_EQ(left.size(), 2048u);

    EXPECT_EQ(right.data(), nullptr);
    EXPECT_EQ(right.stack_bp(), nullptr);
    EXPECT_EQ(right.size(), 0u);
}

TEST_F(SharedStackBufferUnitTest, MoveAssignmentSelfIsNoOp) {
    SharedStackBuffer buffer(256);
    char *data_before = buffer.data();
    char *bp_before = buffer.stack_bp();
    const size_t size_before = buffer.size();

    buffer = std::move(buffer);

    EXPECT_EQ(buffer.data(), data_before);
    EXPECT_EQ(buffer.stack_bp(), bp_before);
    EXPECT_EQ(buffer.size(), size_before);
}

TEST_F(SharedStackBufferUnitTest, OccupyFiberSetterAndGetterWork) {
    SharedStackBuffer buffer(256);

    Fiber *sentinel = reinterpret_cast<Fiber *>(0x1);
    buffer.set_occupy_fiber(sentinel);
    EXPECT_EQ(buffer.occupy_fiber(), sentinel);

    buffer.set_occupy_fiber(nullptr);
    EXPECT_EQ(buffer.occupy_fiber(), nullptr);
}

TEST_F(SharedStackBufferUnitTest, SharedStackPoolAccessAndBounds) {
    SharedStackPool pool(4, 1024);

    EXPECT_EQ(pool.count(), 4u);
    EXPECT_NE(pool.data(0), nullptr);
    EXPECT_EQ(pool.size(0), 1024u);

    EXPECT_EQ(pool.data(9), nullptr);
    EXPECT_EQ(pool.size(9), 0u);
}

TEST_F(SharedStackBufferUnitTest, ConstAccessorsExposeSamePointers) {
    SharedStackBuffer buffer(128);
    const SharedStackBuffer &const_ref = buffer;

    EXPECT_EQ(const_ref.data(), buffer.data());
    EXPECT_EQ(const_ref.stack_bp(), buffer.stack_bp());
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
