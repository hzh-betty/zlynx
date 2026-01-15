/**
 * @file scheduler_test.cc
 * @brief Scheduler类单元测试
 */

#include "runtime/fiber.h"
#include "scheduling/scheduler.h"
#include "util/zcoroutine_logger.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace zcoroutine;

class SchedulerTest : public ::testing::Test {
protected:
  void SetUp() override {}

  void TearDown() override {}
};

// 测试：调度器创建
TEST_F(SchedulerTest, CreateScheduler) {
  Scheduler scheduler(4, "TestScheduler");
  EXPECT_EQ(scheduler.name(), "TestScheduler");
}

// 测试：调度器启动和停止
TEST_F(SchedulerTest, StartAndStop) {
  Scheduler scheduler(2);

  EXPECT_NO_THROW(scheduler.start());
  EXPECT_TRUE(scheduler.is_running());

  EXPECT_NO_THROW(scheduler.stop());
  EXPECT_FALSE(scheduler.is_running());
}

// 测试：调度函数任务
TEST_F(SchedulerTest, ScheduleFunction) {
  Scheduler scheduler(4);
  scheduler.start();

  std::atomic<int> counter{0};

  for (int i = 0; i < 100; ++i) {
    scheduler.schedule([&counter]() { counter++; });
  }

  scheduler.stop();
  EXPECT_EQ(counter.load(), 100);
}

// 测试：调度协程任务
TEST_F(SchedulerTest, ScheduleFiber) {
  Scheduler scheduler(2);
  scheduler.start();

  std::atomic<int> count{0};
  Fiber::ptr fiber;
  for (int i = 0; i < 50; ++i) {
    fiber = std::make_shared<Fiber>([&count]() {
      count++;
      Fiber::yield();
      count++;
    });
    scheduler.schedule(fiber);
  }

  scheduler.stop();
  EXPECT_EQ(count.load(), 50); // 每个协程执行1次
}

// 测试：并发调度
TEST_F(SchedulerTest, ConcurrentSchedule) {
  Scheduler scheduler(4);
  scheduler.start();

  std::atomic<int> sum{0};
  const int task_count = 1000;

  for (int i = 0; i < task_count; ++i) {
    scheduler.schedule([&sum, i]() { sum += i; });
  }

  scheduler.stop();

  // 计算期望值：0 + 1 + 2 + ... + 999
  int expected = (task_count - 1) * task_count / 2;
  EXPECT_EQ(sum.load(), expected);
}

// 测试：多线程调度器
TEST_F(SchedulerTest, MultiThreadScheduler) {
  const int thread_count = 8;
  Scheduler scheduler(thread_count);
  scheduler.start();

  std::atomic<int> executed{0};
  const int task_count = 100;

  for (int i = 0; i < task_count; ++i) {
    scheduler.schedule([&executed]() {
      executed++;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
  }

  scheduler.stop();
  EXPECT_EQ(executed.load(), task_count);
}

// 测试：嵌套任务调度
TEST_F(SchedulerTest, NestedSchedule) {
  Scheduler scheduler(2);
  scheduler.start();

  std::atomic<int> outer{0};
  std::atomic<int> inner{0};

  scheduler.schedule([&scheduler, &outer, &inner]() {
    outer++;
    scheduler.schedule([&inner]() { inner++; });
  });

  scheduler.stop();
  EXPECT_EQ(outer.load(), 1);
  EXPECT_EQ(inner.load(), 1);
}

// 测试：使用bind调度
TEST_F(SchedulerTest, ScheduleWithBind) {
  Scheduler scheduler(2);
  scheduler.start();

  std::atomic<int> result{0};

  auto add_func = [](std::atomic<int> &res, int a, int b) { res = a + b; };

  scheduler.schedule(add_func, std::ref(result), 10, 20);

  scheduler.stop();
  EXPECT_EQ(result.load(), 30);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
