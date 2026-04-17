#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zco {
namespace {

class EventUnitTest : public test::RuntimeTestBase {};

TEST_F(EventUnitTest, ThreadContextAutoResetConsumesCachedSignalOnce) {
    Event event(false, false);

    event.signal();
    EXPECT_TRUE(event.wait(1));
    EXPECT_FALSE(event.wait(5));
}

TEST_F(EventUnitTest, ThreadContextManualResetKeepsSignalUntilReset) {
    Event event(true, true);

    EXPECT_TRUE(event.wait(1));
    EXPECT_TRUE(event.wait(1));

    event.reset();
    EXPECT_FALSE(event.wait(5));
}

TEST_F(EventUnitTest, ThreadContextInfiniteWaitIsReleasedBySignal) {
    Event event(false, false);
    std::atomic<bool> woke(false);

    std::thread waiter([&event, &woke]() {
        const bool ok = event.wait(kInfiniteTimeoutMs);
        woke.store(ok, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    event.signal();
    waiter.join();

    EXPECT_TRUE(woke.load(std::memory_order_acquire));
}

TEST_F(EventUnitTest, ThreadContextAutoResetNotifyAllDoesNotCacheSignal) {
    Event event(false, false);

    event.notify_all();
    EXPECT_FALSE(event.wait(3));
}

TEST_F(EventUnitTest, MovedFromEventHandlesNullImplBranchesSafely) {
    Event source(false, false);
    Event moved(std::move(source));

    EXPECT_FALSE(source.wait(1));
    source.signal();
    source.notify_all();
    source.reset();

    moved.signal();
    EXPECT_TRUE(moved.wait(1));
}

TEST_F(EventUnitTest, CoroutineAutoResetWakesSingleWaiter) {
    init(2);

    Event event(false, false);
    WaitGroup done(2);
    std::atomic<int> started(0);
    std::atomic<int> woke(0);

    for (int i = 0; i < 2; ++i) {
        go([&event, &done, &started, &woke]() {
            started.fetch_add(1, std::memory_order_relaxed);
            if (event.wait(80)) {
                woke.fetch_add(1, std::memory_order_relaxed);
            }
            done.done();
        });
    }

    for (int i = 0; i < 40 && started.load(std::memory_order_relaxed) < 2;
         ++i) {
        co_sleep_for(1);
    }

    event.signal();
    done.wait();

    EXPECT_EQ(woke.load(std::memory_order_relaxed), 1);
}

TEST_F(EventUnitTest, CoroutineManualResetNotifyAllWakesAll) {
    init(3);

    Event event(true, false);
    constexpr int kWaiters = 16;
    WaitGroup done(kWaiters + 1);
    std::atomic<int> woke(0);

    for (int i = 0; i < kWaiters; ++i) {
        go([&event, &done, &woke]() {
            if (event.wait(200)) {
                woke.fetch_add(1, std::memory_order_relaxed);
            }
            done.done();
        });
    }

    go([&event, &done]() {
        co_sleep_for(5);
        event.notify_all();
        done.done();
    });

    done.wait();
    EXPECT_EQ(woke.load(std::memory_order_relaxed), kWaiters);
}

TEST_F(EventUnitTest, CoroutineTimeoutReturnsFalseAndSetsTimeoutFlag) {
    init(1);

    WaitGroup done(1);
    std::atomic<bool> timed_out(false);

    go([&done, &timed_out]() {
        Event event(false, false);
        const bool ok = event.wait(8);
        timed_out.store(!ok && timeout(), std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
}

TEST_F(EventUnitTest, CoroutineAutoResetCachesSignalWithoutWaiter) {
    init(1);

    WaitGroup done(1);
    std::atomic<bool> first_ok(false);
    std::atomic<bool> second_ok(true);

    go([&done, &first_ok, &second_ok]() {
        Event event(false, false);
        event.signal();
        first_ok.store(event.wait(10), std::memory_order_release);
        second_ok.store(event.wait(5), std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_TRUE(first_ok.load(std::memory_order_acquire));
    EXPECT_FALSE(second_ok.load(std::memory_order_acquire));
}

TEST_F(EventUnitTest, CoroutineManualResetSignalKeepsStateUntilReset) {
    init(1);

    WaitGroup done(1);
    std::atomic<bool> first_ok(false);
    std::atomic<bool> second_ok(false);
    std::atomic<bool> after_reset_ok(true);

    go([&done, &first_ok, &second_ok, &after_reset_ok]() {
        Event event(true, false);
        event.signal();
        first_ok.store(event.wait(10), std::memory_order_release);
        second_ok.store(event.wait(10), std::memory_order_release);
        event.reset();
        after_reset_ok.store(event.wait(5), std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_TRUE(first_ok.load(std::memory_order_acquire));
    EXPECT_TRUE(second_ok.load(std::memory_order_acquire));
    EXPECT_FALSE(after_reset_ok.load(std::memory_order_acquire));
}

TEST_F(EventUnitTest, CoroutineAutoResetNotifyAllWakesCurrentWaitersOnly) {
    init(2);

    Event event(false, false);
    WaitGroup done(3);
    std::atomic<int> woke(0);

    go([&event, &done, &woke]() {
        if (event.wait(100)) {
            woke.fetch_add(1, std::memory_order_relaxed);
        }
        done.done();
    });
    go([&event, &done, &woke]() {
        if (event.wait(100)) {
            woke.fetch_add(1, std::memory_order_relaxed);
        }
        done.done();
    });
    go([&event, &done]() {
        co_sleep_for(5);
        event.notify_all();
        done.done();
    });

    done.wait();
    EXPECT_EQ(woke.load(std::memory_order_relaxed), 2);

    // auto-reset notify_all does not cache signaled state.
    EXPECT_FALSE(event.wait(5));
}

TEST_F(EventUnitTest, CopyConstructedEventSharesSignalState) {
    Event original(false, false);
    Event copied(original);

    copied.signal();
    EXPECT_TRUE(original.wait(5));
    EXPECT_FALSE(copied.wait(5));
}

TEST_F(EventUnitTest, ThreadManualResetNotifyAllKeepsSignaledUntilReset) {
    Event event(true, false);
    event.notify_all();
    EXPECT_TRUE(event.wait(5));
    EXPECT_TRUE(event.wait(5));
    event.reset();
    EXPECT_FALSE(event.wait(5));
}

} // namespace
} // namespace zco
