/**
 * @file fiber_pool_test.cc
 * @brief FiberPool 类单元测试
 */

#include "fiber_pool.h"
#include "thread_context.h"
#include "zcoroutine_logger.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace zcoroutine;

class FiberPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 全局一次初始化（重复调用会被忽略）
    FiberPool::get_instance().init(StackAllocator::kDefaultStackSize,
                                   SharedStack::kDefaultStackSize, 1000);

    // 清理当前线程 TLS 池，避免用例相互影响
    ThreadContext::fiber_pool_clear();
  }

  void TearDown() override { ThreadContext::fiber_pool_clear(); }
};

// 测试 1: 从池中获取协程（第一次 - 应该创建新的）
TEST_F(FiberPoolTest, GetFiberFirstTime) {
  auto &pool = FiberPool::get_instance();

  bool executed = false;
  auto fiber = pool.get_fiber([&executed]() { executed = true; });

  EXPECT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  EXPECT_FALSE(executed);

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试 2: 归还协程到池中
TEST_F(FiberPoolTest, ReturnFiber) {
  auto &pool = FiberPool::get_instance();

  auto fiber = pool.get_fiber([]() {});
  fiber->resume();

  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 0);

  bool returned = pool.return_fiber(fiber);
  EXPECT_TRUE(returned);
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 1);
}

// 测试 3: 从池中复用协程
TEST_F(FiberPoolTest, ReuseFiber) {
  auto &pool = FiberPool::get_instance();

  // 创建并返回第一个协程
  auto fiber1 = pool.get_fiber([]() {});
  fiber1->resume();
  pool.return_fiber(fiber1);

  uint64_t fiber1_id = fiber1.get()->id();

  // 获取第二个协程（应该复用 fiber1）
  int count = 0;
  auto fiber2 = pool.get_fiber([&count]() { count++; });

  EXPECT_EQ(fiber2->id(), fiber1_id); // 验证 ID 相同表示复用了同一个对象
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 0); // 获取后池应该为空

  fiber2->resume();
  EXPECT_EQ(count, 1);
  EXPECT_EQ(fiber2->state(), Fiber::State::kTerminated);
}

// 测试 4: 不能归还未终止的协程
TEST_F(FiberPoolTest, CannotReturnNonTerminatedFiber) {
  auto &pool = FiberPool::get_instance();

  auto fiber = pool.get_fiber([&]() { Fiber::yield(); });

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  bool returned = pool.return_fiber(fiber);
  EXPECT_FALSE(returned);
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 0);
}

// 测试 5: 归还空协程
TEST_F(FiberPoolTest, ReturnNullFiber) {
  auto &pool = FiberPool::get_instance();

  Fiber::ptr null_fiber = nullptr;
  bool returned = pool.return_fiber(null_fiber);

  EXPECT_FALSE(returned);
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 0);
}

// 测试 13: 并发获取和归还
TEST_F(FiberPoolTest, ConcurrentGetReturn) {
  auto &pool = FiberPool::get_instance();

  const int thread_count = 4;
  const int operations_per_thread = 50;
  std::vector<std::thread> threads;
  std::atomic<int> total_executed{0};

  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&pool, &total_executed]() {
      for (int i = 0; i < operations_per_thread; ++i) {
        auto fiber = pool.get_fiber([&total_executed]() {
          total_executed.fetch_add(1, std::memory_order_relaxed);
        });

        fiber->resume();
        pool.return_fiber(fiber);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(total_executed.load(), thread_count * operations_per_thread);
}

// 测试 15: 大量协程
TEST_F(FiberPoolTest, ManyFibers) {
  auto &pool = FiberPool::get_instance();

  const int fiber_count = 1000;
  std::atomic<int> counter{0};

  for (int i = 0; i < fiber_count; ++i) {
    auto fiber = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    fiber->resume();
    pool.return_fiber(fiber);
  }

  EXPECT_EQ(counter.load(), fiber_count);
}

// 测试 16: 包含 yield 的协程
TEST_F(FiberPoolTest, FiberWithYield) {
  auto &pool = FiberPool::get_instance();

  int step = 0;
  auto fiber = pool.get_fiber([&step]() {
    step = 1;
    Fiber::yield();
    step = 2;
  });

  fiber->resume();
  EXPECT_EQ(step, 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(step, 2);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  pool.return_fiber(fiber);
  EXPECT_EQ(ThreadContext::fiber_pool_size(), 1);
}

// 测试 18: 单例实例
TEST_F(FiberPoolTest, SingletonInstance) {
  auto &pool1 = FiberPool::get_instance();
  auto &pool2 = FiberPool::get_instance();

  EXPECT_EQ(&pool1, &pool2);
}

int main(int argc, char **argv) {
  // Initialize logger
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
