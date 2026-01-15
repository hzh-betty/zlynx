/**
 * @file fiber_pool_integration_test.cc
 * @brief FiberPool 与调度器的集成测试
 */

#include "runtime/fiber_pool.h"
#include "scheduling/scheduler.h"
#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace zcoroutine;

class FiberPoolIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 全局一次初始化（重复调用会被忽略）
    FiberPool::get_instance().init(StackAllocator::kDefaultStackSize,
                                   SharedStack::kDefaultStackSize, 1000);

    // 清理当前线程 TLS 池，避免用例相互影响
    ThreadContext::fiber_pool_clear();

    // 初始化调度器
    scheduler_ = std::make_shared<Scheduler>(4, "TestScheduler", false);
    scheduler_->start();
  }

  void TearDown() override {
    // 停止调度器
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }

    ThreadContext::fiber_pool_clear();
  }

  Scheduler::ptr scheduler_;
};

// 测试 1: 将协程池与调度器一起使用
TEST_F(FiberPoolIntegrationTest, UseWithScheduler) {
  auto &pool = FiberPool::get_instance();

  std::atomic<int> counter{0};
  const int task_count = 100;

  for (int i = 0; i < task_count; ++i) {
    auto fiber = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });

    scheduler_->schedule(fiber);
  }

  // 等待所有任务完成
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_EQ(counter.load(), task_count);
}

// 测试 2: 高并发下的池复用
TEST_F(FiberPoolIntegrationTest, HighConcurrencyReuse) {
  auto &pool = FiberPool::get_instance();

  std::atomic<int> completed{0};
  const int total_tasks = 500;

  for (int i = 0; i < total_tasks; ++i) {
    auto fiber = pool.get_fiber([&completed]() {
      // 模拟一些工作
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      completed.fetch_add(1, std::memory_order_relaxed);
    });

    scheduler_->schedule(std::move(fiber));
  }

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::seconds(2));

  EXPECT_EQ(completed.load(), total_tasks);
}

// 测试 3: 混合协程类型（来自池和直接创建）
TEST_F(FiberPoolIntegrationTest, MixedFiberTypes) {
  auto &pool = FiberPool::get_instance();

  std::atomic<int> pool_fiber_count{0};
  std::atomic<int> direct_fiber_count{0};
  const int each_count = 50;

  // 从池中调度协程
  for (int i = 0; i < each_count; ++i) {
    auto fiber = pool.get_fiber([&pool_fiber_count]() {
      pool_fiber_count.fetch_add(1, std::memory_order_relaxed);
    });
    scheduler_->schedule(std::move(fiber));
  }

  // 调度直接创建的协程
  for (int i = 0; i < each_count; ++i) {
    auto fiber = std::make_shared<Fiber>([&direct_fiber_count]() {
      direct_fiber_count.fetch_add(1, std::memory_order_relaxed);
    });
    scheduler_->schedule(std::move(fiber));
  }

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_EQ(pool_fiber_count.load(), each_count);
  EXPECT_EQ(direct_fiber_count.load(), each_count);
}

// 测试 4: 压力测试 - 快速协程创建和执行
TEST_F(FiberPoolIntegrationTest, StressTest) {
  auto &pool = FiberPool::get_instance();

  std::atomic<uint64_t> sum{0};
  const int iteration_count = 1000;

  for (int i = 0; i < iteration_count; ++i) {
    auto fiber = pool.get_fiber(
        [&sum, i]() { sum.fetch_add(i, std::memory_order_relaxed); });
    scheduler_->schedule(fiber);
  }

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 计算期望和: 0 + 1 + 2 + ... + (n-1) = n*(n-1)/2
  uint64_t expected_sum =
      static_cast<uint64_t>(iteration_count) * (iteration_count - 1) / 2;
  EXPECT_EQ(sum.load(), expected_sum);

  ZCOROUTINE_LOG_INFO("Stress test completed: {} iterations, hit rate: {:.2f}%",
                      iteration_count, 0.0);
}

// 测试 5: 包含 yield 协程的协程池
TEST_F(FiberPoolIntegrationTest, YieldingFibers) {
  auto &pool = FiberPool::get_instance();

  std::atomic<int> phase1_count{0};
  std::atomic<int> phase2_count{0};
  const int fiber_count = 50;

  std::vector<Fiber::ptr> fibers;

  for (int i = 0; i < fiber_count; ++i) {
    auto fiber = pool.get_fiber([&phase1_count, &phase2_count]() {
      phase1_count.fetch_add(1, std::memory_order_relaxed);
      Fiber::yield();
      phase2_count.fetch_add(1, std::memory_order_relaxed);
    });

    fibers.push_back(fiber);
    scheduler_->schedule(fiber);
  }

  // 等待阶段 1
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 恢复所有协程进行阶段 2
  for (auto &fiber : fibers) {
    scheduler_->schedule(fiber);
  }

  // 等待阶段 2
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_EQ(phase1_count.load(), fiber_count);
  EXPECT_EQ(phase2_count.load(), fiber_count);
}

// 测试 6: 内存压力下的池行为
TEST_F(FiberPoolIntegrationTest, MemoryPressure) {
  auto &pool = FiberPool::get_instance();

  std::atomic<int> executed{0};
  const int large_task_count = 200;

  for (int i = 0; i < large_task_count; ++i) {
    auto fiber = pool.get_fiber(
        [&executed]() { executed.fetch_add(1, std::memory_order_relaxed); });
    scheduler_->schedule(fiber);

    // 小延迟以允许一些协程完成
    if (i % 20 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // 等待全部完成
  std::this_thread::sleep_for(std::chrono::seconds(2));

  EXPECT_EQ(executed.load(), large_task_count);
}

// 测试 7: 池性能比较
TEST_F(FiberPoolIntegrationTest, PerformanceComparison) {
  const int task_count = 500;

  // 使用池进行测试
  auto &pool = FiberPool::get_instance();

  auto start_pool = std::chrono::steady_clock::now();
  std::atomic<int> pool_counter{0};

  for (int i = 0; i < task_count; ++i) {
    auto fiber = pool.get_fiber([&pool_counter]() {
      pool_counter.fetch_add(1, std::memory_order_relaxed);
    });
    scheduler_->schedule(fiber);
  }

  while (pool_counter.load() < task_count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto end_pool = std::chrono::steady_clock::now();
  auto duration_pool = std::chrono::duration_cast<std::chrono::milliseconds>(
                           end_pool - start_pool)
                           .count();

  auto start_direct = std::chrono::steady_clock::now();
  std::atomic<int> direct_counter{0};

  for (int i = 0; i < task_count; ++i) {
    auto fiber = std::make_shared<Fiber>([&direct_counter]() {
      direct_counter.fetch_add(1, std::memory_order_relaxed);
    });
    scheduler_->schedule(fiber);
  }

  // 等待完成
  while (direct_counter.load() < task_count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto end_direct = std::chrono::steady_clock::now();
  auto duration_direct = std::chrono::duration_cast<std::chrono::milliseconds>(
                             end_direct - start_direct)
                             .count();

  ZCOROUTINE_LOG_INFO("Performance comparison ({} tasks):", task_count);
  ZCOROUTINE_LOG_INFO("  With pool:    {} ms (hit rate: {:.2f}%)",
                      duration_pool, 0.0);
  ZCOROUTINE_LOG_INFO("  Without pool: {} ms", duration_direct);
}

// 测试 8: 多个调度器的并发池访问
TEST_F(FiberPoolIntegrationTest, MultipleSchedulers) {
  auto &pool = FiberPool::get_instance();

  // 创建额外的调度器
  auto scheduler2 = std::make_shared<Scheduler>(2, "Scheduler2", false);
  auto scheduler3 = std::make_shared<Scheduler>(2, "Scheduler3", false);

  scheduler2->start();
  scheduler3->start();

  std::atomic<int> counter{0};
  const int tasks_per_scheduler = 100;

  // 在多个调度器上调度任务
  for (int i = 0; i < tasks_per_scheduler; ++i) {
    auto fiber1 = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    scheduler_->schedule(fiber1);

    auto fiber2 = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    scheduler2->schedule(fiber2);

    auto fiber3 = pool.get_fiber(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    scheduler3->schedule(fiber3);
  }

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::seconds(2));

  EXPECT_EQ(counter.load(), tasks_per_scheduler * 3);

  // 清理
  scheduler2->stop();
  scheduler3->stop();
}

int main(int argc, char **argv) {
  // 初始化日志
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
