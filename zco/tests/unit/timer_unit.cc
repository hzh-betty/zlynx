#include <atomic>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/timer.h"

namespace zco {
namespace {

class TimerUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(TimerUnitByHeaderTest, NowMsIsMonotonic) {
    const uint64_t begin = now_ms();
    uint64_t end = begin;

    for (int i = 0; i < 10000 && end == begin; ++i) {
        end = now_ms();
    }

    EXPECT_GE(end, begin);
}

TEST_F(TimerUnitByHeaderTest, AddTimerAndProcessDueExecutesCallback) {
    TimerQueue queue;
    std::atomic<bool> fired(false);

    std::shared_ptr<TimerToken> token =
        queue.add_timer(1, [&fired]() { fired.store(true, std::memory_order_release); });
    ASSERT_NE(token, nullptr);

    const uint64_t deadline = now_ms() + 100;
    while (!fired.load(std::memory_order_acquire) && now_ms() < deadline) {
        queue.process_due();
    }

    EXPECT_TRUE(fired.load(std::memory_order_acquire));
}

TEST_F(TimerUnitByHeaderTest, CancelledTimerIsSkippedDuringProcessDue) {
    TimerQueue queue;
    std::atomic<bool> fired(false);

    std::shared_ptr<TimerToken> token =
        queue.add_timer(1, [&fired]() { fired.store(true, std::memory_order_release); });
    ASSERT_NE(token, nullptr);
    token->cancelled.store(true, std::memory_order_release);

    const uint64_t deadline = now_ms() + 100;
    while (now_ms() < deadline) {
        queue.process_due();
    }

    EXPECT_FALSE(fired.load(std::memory_order_acquire));
}

TEST_F(TimerUnitByHeaderTest, EmptyAndFutureQueueTimeoutsFollowContract) {
    TimerQueue queue;
    EXPECT_EQ(queue.next_timeout_ms(), 1000);

    queue.add_timer(5000, []() {});
    const int timeout_ms = queue.next_timeout_ms();
    EXPECT_GE(timeout_ms, 0);
    EXPECT_LE(timeout_ms, 1000);
}

TEST_F(TimerUnitByHeaderTest, DueTimerMakesNextTimeoutZeroAndNullCallbackIsSafe) {
    TimerQueue queue;
    std::shared_ptr<TimerToken> token = queue.add_timer(0, std::function<void()>());
    ASSERT_NE(token, nullptr);

    EXPECT_EQ(queue.next_timeout_ms(), 0);
    queue.process_due();
    EXPECT_EQ(queue.next_timeout_ms(), 1000);
}

} // namespace
} // namespace zco
