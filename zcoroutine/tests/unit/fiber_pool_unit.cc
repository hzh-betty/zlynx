#include <gtest/gtest.h>

#include "zcoroutine/internal/fiber_pool.h"
#include "zcoroutine/internal/processor.h"
#include "support/internal_fiber_test_helper.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class FiberPoolUnitTest : public test::RuntimeTestBase {};

TEST_F(FiberPoolUnitTest, RecycleRespectsCapacityAndAcquireOrder) {
  FiberPool pool(2);
  Processor processor(0, 64 * 1024);

  std::shared_ptr<Fiber> f1 = test::MakeFiberForTest(&processor, 1, 0);
  std::shared_ptr<Fiber> f2 = test::MakeFiberForTest(&processor, 2, 1);
  std::shared_ptr<Fiber> f3 = test::MakeFiberForTest(&processor, 3, 2);

  pool.recycle(nullptr);
  EXPECT_EQ(pool.size(), 0u);

  pool.recycle(f1);
  pool.recycle(f2);
  pool.recycle(f3);
  EXPECT_EQ(pool.size(), 2u);

  std::shared_ptr<Fiber> a = pool.acquire();
  std::shared_ptr<Fiber> b = pool.acquire();
  std::shared_ptr<Fiber> c = pool.acquire();

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
    pool.recycle(test::MakeFiberForTest(&processor, i + 1, static_cast<size_t>(i % 4)));
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

}  // namespace
}  // namespace zcoroutine
