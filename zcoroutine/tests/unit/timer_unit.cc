#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "zcoroutine/internal/timer.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class TimerUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(TimerUnitByHeaderTest, AddProcessAndCancelFlow) {
  TimerQueue queue;
  std::atomic<int> called(0);

  queue.add_timer(0, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
  std::shared_ptr<TimerToken> cancelled =
      queue.add_timer(0, [&called]() { called.fetch_add(100, std::memory_order_relaxed); });
  cancelled->cancelled.store(true, std::memory_order_release);

  queue.process_due();
  EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

TEST_F(TimerUnitByHeaderTest, NextTimeoutTransitionsFromFutureToDue) {
  TimerQueue queue;
  queue.add_timer(20, []() {});

  const int before = queue.next_timeout_ms();
  EXPECT_GT(before, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  EXPECT_EQ(queue.next_timeout_ms(), 0);
}

TEST_F(TimerUnitByHeaderTest, NextTimeoutEmptyQueueReturnsFallback) {
  TimerQueue queue;
  EXPECT_EQ(queue.next_timeout_ms(), 1000);
}

TEST_F(TimerUnitByHeaderTest, NextTimeoutIsCappedWhenDeadlineIsFar) {
  TimerQueue queue;
  queue.add_timer(5000, []() {});

  EXPECT_EQ(queue.next_timeout_ms(), 1000);
}

TEST_F(TimerUnitByHeaderTest, ProcessDueHandlesNullCallbackEntry) {
  TimerQueue queue;
  queue.add_timer(0, std::function<void()>());

  queue.process_due();
  EXPECT_EQ(queue.next_timeout_ms(), 1000);
}

}  // namespace
}  // namespace zcoroutine
