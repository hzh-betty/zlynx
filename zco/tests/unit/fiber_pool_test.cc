#include <gtest/gtest.h>

#include "support/internal_fiber_test_helper.h"
#include "support/test_fixture.h"
#include "zco/internal/fiber_pool.h"
#include "zco/internal/processor.h"

namespace zco {
namespace {

class FiberPoolUnitTest : public test::RuntimeTestBase {};

TEST_F(FiberPoolUnitTest, RecycleRespectsCapacityAndAcquireOrder) {
    FiberPool pool(2);
    Processor processor(0, 64 * 1024);

    Fiber::ptr f1 = test::MakeFiberForTest(&processor, 1, 0);
    Fiber::ptr f2 = test::MakeFiberForTest(&processor, 2, 1);
    Fiber::ptr f3 = test::MakeFiberForTest(&processor, 3, 2);

    pool.recycle(nullptr);
    EXPECT_EQ(pool.size(), 0u);

    pool.recycle(f1);
    pool.recycle(f2);
    pool.recycle(f3);
    EXPECT_EQ(pool.size(), 2u);

    Fiber::ptr a = pool.acquire();
    Fiber::ptr b = pool.acquire();
    Fiber::ptr c = pool.acquire();

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(c, nullptr);

    EXPECT_EQ(a->id(), 1);
    EXPECT_EQ(b->id(), 2);
}

TEST_F(FiberPoolUnitTest, ClearRemovesAllCachedFibers) {
    FiberPool pool(8);
    Processor processor(0, 64 * 1024);

    for (int i = 0; i < 6; ++i) {
        pool.recycle(test::MakeFiberForTest(&processor, i + 1,
                                            static_cast<size_t>(i % 4)));
    }

    EXPECT_EQ(pool.size(), 6u);
    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);
}

TEST_F(FiberPoolUnitTest, ZeroCapacityPoolRejectsRecycle) {
    FiberPool pool(0);
    Processor processor(0, 64 * 1024);

    pool.recycle(test::MakeFiberForTest(&processor, 9, 0));
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
