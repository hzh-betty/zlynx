#include <atomic>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/wait_group.h"

namespace zco {
namespace {

class WaitGroupUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(WaitGroupUnitByHeaderTest, AddDoneAndWaitFlow) {
    WaitGroup group(0);
    group.add(2);

    go([&group]() {
        co_sleep_for(1);
        group.done();
    });

    go([&group]() {
        co_sleep_for(1);
        group.done();
    });

    group.wait();
    SUCCEED();
}

TEST_F(WaitGroupUnitByHeaderTest, DoneWithoutOutstandingCountThrows) {
    WaitGroup group(0);
    EXPECT_THROW(group.done(), std::runtime_error);
}

TEST_F(WaitGroupUnitByHeaderTest, AddZeroKeepsWaitImmediatelyReady) {
    WaitGroup group(0);

    group.add(0);
    group.wait();
    SUCCEED();
}

TEST_F(WaitGroupUnitByHeaderTest, AddFromZeroBlocksUntilDoneThenCanReuse) {
    WaitGroup group(0);
    std::atomic<bool> waited(false);

    group.add(1);
    Event waiter_entered(false, false);
    std::thread waiter([&group, &waited, &waiter_entered]() {
        waiter_entered.signal();
        group.wait();
        waited.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(waiter_entered.wait(1000));
    EXPECT_FALSE(waited.load(std::memory_order_acquire));

    group.done();
    waiter.join();
    EXPECT_TRUE(waited.load(std::memory_order_acquire));

    // 计数归零后应可继续复用。
    waited.store(false, std::memory_order_release);
    group.add(1);
    Event waiter2_entered(false, false);
    std::thread waiter2([&group, &waited, &waiter2_entered]() {
        waiter2_entered.signal();
        group.wait();
        waited.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(waiter2_entered.wait(1000));
    EXPECT_FALSE(waited.load(std::memory_order_acquire));
    group.done();
    waiter2.join();
    EXPECT_TRUE(waited.load(std::memory_order_acquire));
}

TEST_F(WaitGroupUnitByHeaderTest, ConcurrentDoneWakesWaitAndGroupCanBeReused) {
    init(4);

    constexpr int kWorkers = 32;
    WaitGroup group(0);
    group.add(kWorkers);

    Event start_gate(true, false);
    WaitGroup ready(kWorkers);
    WaitGroup finished(kWorkers + 1);

    std::atomic<int> done_calls(0);
    std::atomic<bool> wait_released(false);

    for (int i = 0; i < kWorkers; ++i) {
        go([&group, &start_gate, &ready, &finished, &done_calls]() {
            ready.done();
            EXPECT_TRUE(start_gate.wait(2000));
            group.done();
            done_calls.fetch_add(1, std::memory_order_relaxed);
            finished.done();
        });
    }

    go([&group, &finished, &wait_released]() {
        group.wait();
        wait_released.store(true, std::memory_order_release);
        finished.done();
    });

    ready.wait();
    EXPECT_FALSE(wait_released.load(std::memory_order_acquire));

    start_gate.signal();
    finished.wait();

    EXPECT_TRUE(wait_released.load(std::memory_order_acquire));
    EXPECT_EQ(done_calls.load(std::memory_order_relaxed), kWorkers);

    group.add(1);
    go([&group]() { group.done(); });
    group.wait();
}

} // namespace
} // namespace zco
