#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "zcoroutine/pool.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class PoolUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(PoolUnitByHeaderTest, CreatePopPushAndDestroyRespectCapacity) {
  std::atomic<int> created(0);
  std::atomic<int> destroyed(0);

  Pool pool(
      [&created]() -> void* {
        created.fetch_add(1, std::memory_order_relaxed);
        return new int(7);
      },
      [&destroyed](void* ptr) {
        destroyed.fetch_add(1, std::memory_order_relaxed);
        delete static_cast<int*>(ptr);
      },
      1);

  void* first = pool.pop();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(*static_cast<int*>(first), 7);
  EXPECT_EQ(created.load(std::memory_order_relaxed), 1);

  pool.push(first);
  EXPECT_EQ(pool.size(), 1u);

  int* overflow = new int(9);
  pool.push(overflow);
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);

  pool.clear();
  EXPECT_EQ(pool.size(), 0u);
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 2);
}

TEST_F(PoolUnitByHeaderTest, PoolGuardAutomaticallyReturnsObject) {
  Pool pool(
      []() -> void* { return new int(42); },
      [](void* ptr) { delete static_cast<int*>(ptr); },
      8);

  {
    PoolGuard<int> guard(pool);
    ASSERT_TRUE(static_cast<bool>(guard));
    EXPECT_EQ(*guard, 42);
  }

  EXPECT_EQ(pool.size(), 1u);
}

TEST_F(PoolUnitByHeaderTest, PopWithoutCreateAndPushNullAreNoop) {
  Pool pool(nullptr, nullptr, 4);

  EXPECT_EQ(pool.pop(), nullptr);
  pool.push(nullptr);
  EXPECT_EQ(pool.size(), 0u);
}

TEST_F(PoolUnitByHeaderTest, BucketsAreThreadLocalAndClearReleasesAll) {
  std::atomic<int> destroyed(0);
  Pool pool(
      nullptr,
      [&destroyed](void* ptr) {
        destroyed.fetch_add(1, std::memory_order_relaxed);
        delete static_cast<int*>(ptr);
      },
      8);

  std::thread worker([&pool]() {
    pool.push(new int(11));
    EXPECT_EQ(pool.size(), 1u);
  });
  worker.join();

  // 主线程访问的是另一个分桶，因此看不到子线程缓存对象。
  EXPECT_EQ(pool.size(), 0u);

  pool.clear();
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
}

}  // namespace
}  // namespace zcoroutine
