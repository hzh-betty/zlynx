#include "runtime/fiber.h"
#include "scheduling/scheduler.h"
#include "timer/timer_manager.h"
#include "util/zcoroutine_logger.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace zcoroutine;

class TimerSchedulerIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    timer_manager_ = std::make_unique<TimerManager>();
    scheduler_ = std::make_shared<Scheduler>(2, "TimerScheduler");
  }

  void TearDown() override {
    if (scheduler_ && scheduler_->is_running()) {
      scheduler_->stop();
    }
    scheduler_.reset();
    timer_manager_.reset();
  }

  std::unique_ptr<TimerManager> timer_manager_;
  Scheduler::ptr scheduler_;
};

// ==================== 定时器与调度器集成测试 ====================

// 测试1：定时器触发调度协程
TEST_F(TimerSchedulerIntegrationTest, TimerTriggersScheduledFiber) {
  std::atomic<bool> executed{false};

  scheduler_->start();

  timer_manager_->add_timer(
      50,
      [this, &executed]() {
        scheduler_->schedule([&executed]() { executed.store(true); });
      },
      false);

  // 等待定时器触发
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(executed.load());

  scheduler_->stop();
}

// 测试2：循环定时器持续调度任务
TEST_F(TimerSchedulerIntegrationTest, RecurringTimerSchedulesTasks) {
  std::atomic<int> count{0};

  scheduler_->start();

  timer_manager_->add_timer(
      30,
      [this, &count]() {
        scheduler_->schedule([&count]() { count.fetch_add(1); });
      },
      true);

  // 运行一段时间
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  EXPECT_GE(count.load(), 5);

  scheduler_->stop();
}

// 测试3：多个定时器调度不同任务
TEST_F(TimerSchedulerIntegrationTest, MultipleTimersScheduleDifferentTasks) {
  std::atomic<int> task1_count{0};
  std::atomic<int> task2_count{0};
  std::atomic<int> task3_count{0};

  scheduler_->start();

  timer_manager_->add_timer(
      30,
      [this, &task1_count]() {
        scheduler_->schedule([&task1_count]() { task1_count.fetch_add(1); });
      },
      false);

  timer_manager_->add_timer(
      50,
      [this, &task2_count]() {
        scheduler_->schedule([&task2_count]() { task2_count.fetch_add(1); });
      },
      false);

  timer_manager_->add_timer(
      70,
      [this, &task3_count]() {
        scheduler_->schedule([&task3_count]() { task3_count.fetch_add(1); });
      },
      false);

  // 运行足够长时间让所有定时器触发
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(task1_count.load(), 1);
  EXPECT_EQ(task2_count.load(), 1);
  EXPECT_EQ(task3_count.load(), 1);

  scheduler_->stop();
}

// 测试4：取消定时器停止任务调度
TEST_F(TimerSchedulerIntegrationTest, CancelTimerStopsScheduling) {
  std::atomic<int> count{0};

  scheduler_->start();

  auto timer = timer_manager_->add_timer(
      30,
      [this, &count]() {
        scheduler_->schedule([&count]() { count.fetch_add(1); });
      },
      true);

  // 让定时器触发几次
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  // 等待直到 count >= 3 或超时
  for (int i = 0; i < 50 && count.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  int count_before_cancel = count.load();
  EXPECT_GT(count_before_cancel, 0);

  // 取消定时器
  timer->cancel();

  // 继续运行
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 计数不应该继续增加
  EXPECT_EQ(count.load(), count_before_cancel);

  scheduler_->stop();
}

// 测试5：定时器调度带yield的协程
TEST_F(TimerSchedulerIntegrationTest, TimerSchedulesFiberWithYield) {
  std::atomic<int> step{0};

  scheduler_->start();

  timer_manager_->add_timer(
      50,
      [this, &step]() {
        auto fiber = std::make_shared<Fiber>([&step]() {
          step.store(1);
          Fiber::yield();
        });
        scheduler_->schedule(fiber);
      },
      false);

  // 等待定时器触发和协程执行
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(step.load(), 1);

  scheduler_->stop();
}

// 测试6：高频定时器与调度器
TEST_F(TimerSchedulerIntegrationTest, HighFrequencyTimerScheduling) {
  std::atomic<int> count{0};

  scheduler_->start();

  // 10ms的高频定时器
  timer_manager_->add_timer(
      10,
      [this, &count]() {
        scheduler_->schedule([&count]() { count.fetch_add(1); });
      },
      true);

  // 运行200ms
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 应该触发多次
  EXPECT_GE(count.load(), 10);

  scheduler_->stop();
}

// 测试7：定时器在调度器停止后的行为
TEST_F(TimerSchedulerIntegrationTest, TimerBehaviorAfterSchedulerStop) {
  std::atomic<int> count{0};

  scheduler_->start();

  timer_manager_->add_timer(
      30,
      [this, &count]() {
        if (scheduler_->is_running()) {
          scheduler_->schedule([&count]() { count.fetch_add(1); });
        }
      },
      true);

  // 让定时器触发几次
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  // 等待直到 count >= 3 或超时
  for (int i = 0; i < 50 && count.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  int count_before_stop = count.load();
  EXPECT_GT(count_before_stop, 0);

  scheduler_->stop();

  // 定时器仍然会触发，但不会调度任务
  for (int i = 0; i < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 调度器停止后不应该再增加
  EXPECT_EQ(count.load(), count_before_stop);
}

// 测试8：大量定时器同时调度任务
TEST_F(TimerSchedulerIntegrationTest, MassiveTimersScheduling) {
  const int timer_count = 100;
  std::atomic<int> completed{0};

  scheduler_->start();

  for (int i = 0; i < timer_count; ++i) {
    timer_manager_->add_timer(
        50 + (i % 50),
        [this, &completed]() {
          scheduler_->schedule([&completed]() { completed.fetch_add(1); });
        },
        false);
  }

  // 运行足够长时间
  for (int i = 0; i < 15; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(completed.load(), timer_count);

  scheduler_->stop();
}

// 测试9：定时器重置后重新调度
TEST_F(TimerSchedulerIntegrationTest, TimerResetAndReschedule) {
  std::atomic<int> count{0};

  scheduler_->start();

  auto timer = timer_manager_->add_timer(
      100,
      [this, &count]() {
        scheduler_->schedule([&count]() { count.fetch_add(1); });
      },
      false);

  // 等待一半时间
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 重置定时器（延长时间）
  timer->reset(200);

  // 继续等待原来的时间
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  auto callbacks = timer_manager_->list_expired_callbacks();
  for (auto &cb : callbacks) {
    cb();
  }

  // 此时不应该触发
  EXPECT_EQ(count.load(), 0);

  // 等待更长时间
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  callbacks = timer_manager_->list_expired_callbacks();
  for (auto &cb : callbacks) {
    cb();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 现在应该触发了
  EXPECT_EQ(count.load(), 1);

  scheduler_->stop();
}

// 测试10：混合一次性和循环定时器调度
TEST_F(TimerSchedulerIntegrationTest, MixedOneshotAndRecurringTimers) {
  std::atomic<int> oneshot_count{0};
  std::atomic<int> recurring_count{0};

  scheduler_->start();

  // 一次性定时器
  for (int i = 0; i < 5; ++i) {
    timer_manager_->add_timer(
        50,
        [this, &oneshot_count]() {
          scheduler_->schedule(
              [&oneshot_count]() { oneshot_count.fetch_add(1); });
        },
        false);
  }

  // 循环定时器
  for (int i = 0; i < 2; ++i) {
    timer_manager_->add_timer(
        50,
        [this, &recurring_count]() {
          scheduler_->schedule(
              [&recurring_count]() { recurring_count.fetch_add(1); });
        },
        true);
  }

  // 运行多轮
  for (int round = 0; round < 5; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto callbacks = timer_manager_->list_expired_callbacks();
    for (auto &cb : callbacks) {
      cb();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(oneshot_count.load(), 5);
  EXPECT_GE(recurring_count.load(), 4); // 至少2个定时器各触发2次

  scheduler_->stop();
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}