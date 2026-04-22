#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/timer.h"

namespace zco {
namespace {

class TimerQueueUnitTest : public test::RuntimeTestBase {};

TEST_F(TimerQueueUnitTest, NextTimeoutEmptyQueueReturnsFallback) {
    TimerQueue queue;
    EXPECT_EQ(queue.next_timeout_ms(), 1000);
}

TEST_F(TimerQueueUnitTest, DueCancelledAndNullCallbackBranches) {
    TimerQueue queue;
    std::atomic<int> called(0);

    queue.add_timer(
        0, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
    queue.add_timer(0, std::function<void()>());
    std::shared_ptr<TimerToken> cancelled = queue.add_timer(
        0, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
    cancelled->cancelled.store(true, std::memory_order_release);

    EXPECT_EQ(queue.next_timeout_ms(), 0);
    queue.process_due();

    EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

TEST_F(TimerQueueUnitTest, TimeoutIsClampedToOneSecondForFarFutureTimer) {
    TimerQueue queue;
    queue.add_timer(3000, []() {});

    const int timeout_ms = queue.next_timeout_ms();
    EXPECT_EQ(timeout_ms, 1000);
}

TEST_F(TimerQueueUnitTest, NearFutureTimeoutStaysWithinZeroToOneSecond) {
    TimerQueue queue;
    queue.add_timer(40, []() {});

    const int timeout_ms = queue.next_timeout_ms();
    EXPECT_GT(timeout_ms, 0);
    EXPECT_LE(timeout_ms, 1000);
}

TEST_F(TimerQueueUnitTest, ProcessDueReturnsWhenHeadNotExpiredYet) {
    TimerQueue queue;
    std::atomic<int> called(0);

    queue.add_timer(
        30, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(35));

    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), 1);
}

TEST_F(TimerQueueUnitTest, EqualDeadlineMaintainsInsertionOrderBySequence) {
    TimerQueue queue;
    std::vector<int> order;

    queue.add_timer(0, [&order]() { order.push_back(1); });
    queue.add_timer(0, [&order]() { order.push_back(2); });

    queue.process_due();

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST_F(TimerQueueUnitTest, ProcessDueConsumesOnlyExpiredTimers) {
    TimerQueue queue;
    std::atomic<int> called(0);

    queue.add_timer(
        0, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
    queue.add_timer(
        80, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });

    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), 1);

    const int timeout_ms = queue.next_timeout_ms();
    EXPECT_GT(timeout_ms, 0);
    EXPECT_LE(timeout_ms, 1000);

    std::this_thread::sleep_for(std::chrono::milliseconds(85));
    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), 2);
}

TEST_F(TimerQueueUnitTest, CallbackCanScheduleAnotherImmediateTimer) {
    TimerQueue queue;
    std::atomic<int> called(0);

    queue.add_timer(0, [&queue, &called]() {
        called.fetch_add(1, std::memory_order_relaxed);
        queue.add_timer(
            0, [&called]() { called.fetch_add(1, std::memory_order_relaxed); });
    });

    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), 2);
}

TEST_F(TimerQueueUnitTest, ConcurrentAddThenProcessExecutesAllCallbacks) {
    TimerQueue queue;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    constexpr int kTotal = kThreads * kPerThread;

    std::atomic<int> called(0);
    std::vector<std::thread> producers;
    producers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&queue, &called]() {
            for (int i = 0; i < kPerThread; ++i) {
                queue.add_timer(0, [&called]() {
                    called.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    queue.process_due();
    EXPECT_EQ(called.load(std::memory_order_relaxed), kTotal);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
