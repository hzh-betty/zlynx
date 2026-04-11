#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zcoroutine/mutex.h"

namespace zcoroutine {
namespace {

class MutexUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(MutexUnitByHeaderTest, LockUnlockAndTryLockFlow) {
    Mutex mutex;

    mutex.lock();
    EXPECT_FALSE(mutex.try_lock());
    mutex.unlock();

    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST_F(MutexUnitByHeaderTest, MixedThreadAndCoroutineContention) {
    init(4);

    Mutex mutex;
    constexpr int kCoroutineTasks = 80;
    constexpr int kThreadTasks = 40;
    constexpr int kTotal = kCoroutineTasks + kThreadTasks;

    WaitGroup done(kTotal);
    std::atomic<int> counter(0);

    for (int i = 0; i < kCoroutineTasks; ++i) {
        go([&]() {
            MutexGuard guard(mutex);
            counter.fetch_add(1, std::memory_order_relaxed);
            done.done();
        });
    }

    std::vector<std::thread> threads;
    threads.reserve(kThreadTasks);
    for (int i = 0; i < kThreadTasks; ++i) {
        threads.emplace_back([&]() {
            MutexGuard guard(mutex);
            counter.fetch_add(1, std::memory_order_relaxed);
            done.done();
        });
    }

    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), kTotal);
}

TEST_F(MutexUnitByHeaderTest, UnlockWhenNotLockedIsIgnored) {
    Mutex mutex;

    mutex.unlock();
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST_F(MutexUnitByHeaderTest, GuardWithNullPointerIsSafeNoop) {
    MutexGuard guard(static_cast<const Mutex *>(nullptr));
    SUCCEED();
}

TEST_F(MutexUnitByHeaderTest, CoroutineWaiterGetsLockBeforeThreadWaiter) {
    init(1);

    Mutex mutex;
    mutex.lock();

    WaitGroup coroutine_started(1);
    WaitGroup done(2);
    std::vector<int> order;
    std::mutex order_mutex;

    go([&]() {
        coroutine_started.done();
        MutexGuard guard(mutex);
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(1);
        }
        done.done();
    });

    coroutine_started.wait();

    std::thread thread_waiter([&]() {
        MutexGuard guard(mutex);
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(2);
        }
        done.done();
    });

    mutex.unlock();
    done.wait();
    thread_waiter.join();

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

} // namespace
} // namespace zcoroutine
