#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/mutex.h"

namespace zco {
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
    std::atomic<int> counter(0);
    std::atomic<int> completed(0);
    std::atomic<bool> timeout_or_error(false);

    auto finish_task = [&]() {
        completed.fetch_add(1, std::memory_order_release);
    };

    for (int i = 0; i < kCoroutineTasks; ++i) {
        go([&]() {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                if (mutex.try_lock()) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                    mutex.unlock();
                    finish_task();
                    return;
                }
                co_sleep_for(1);
            }

            timeout_or_error.store(true, std::memory_order_release);
            finish_task();
        });
    }

    std::vector<std::thread> threads;
    threads.reserve(kThreadTasks);
    for (int i = 0; i < kThreadTasks; ++i) {
        threads.emplace_back([&]() {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                if (mutex.try_lock()) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                    mutex.unlock();
                    finish_task();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            timeout_or_error.store(true, std::memory_order_release);
            finish_task();
        });
    }

    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    const auto wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed.load(std::memory_order_acquire) < kTotal &&
           std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(completed.load(std::memory_order_acquire), kTotal);
    EXPECT_FALSE(timeout_or_error.load(std::memory_order_acquire));
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

TEST_F(MutexUnitByHeaderTest, GuardWithValidPointerLocksAndUnlocks) {
    Mutex mutex;
    {
        MutexGuard guard(&mutex);
        EXPECT_FALSE(mutex.try_lock());
    }
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
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

TEST_F(MutexUnitByHeaderTest, CoroutineLockUnlockFastPathWorksWhenUnlocked) {
    init(1);

    Mutex mutex;
    WaitGroup done(1);
    std::atomic<bool> got_lock(false);

    go([&]() {
        mutex.lock();
        got_lock.store(true, std::memory_order_release);
        mutex.unlock();
        done.done();
    });

    done.wait();
    EXPECT_TRUE(got_lock.load(std::memory_order_acquire));
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST_F(MutexUnitByHeaderTest,
       ThreadWaiterCanProceedAfterCoroutineHandoffCompletes) {
    init(1);

    Mutex mutex;
    mutex.lock();

    WaitGroup coroutine_started(1);
    WaitGroup done(2);
    std::atomic<bool> coroutine_done(false);
    std::atomic<bool> thread_done(false);

    go([&]() {
        coroutine_started.done();
        MutexGuard guard(mutex);
        coroutine_done.store(true, std::memory_order_release);
        done.done();
    });

    coroutine_started.wait();
    std::thread waiter([&]() {
        MutexGuard guard(mutex);
        thread_done.store(true, std::memory_order_release);
        done.done();
    });

    mutex.unlock();
    done.wait();
    waiter.join();

    EXPECT_TRUE(coroutine_done.load(std::memory_order_acquire));
    EXPECT_TRUE(thread_done.load(std::memory_order_acquire));
}

TEST_F(MutexUnitByHeaderTest, ThreadLockBlocksUntilOwnerUnlocks) {
    Mutex mutex;
    mutex.lock();

    std::atomic<bool> thread_acquired(false);
    std::thread waiter([&]() {
        mutex.lock();
        thread_acquired.store(true, std::memory_order_release);
        mutex.unlock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_FALSE(thread_acquired.load(std::memory_order_acquire));

    mutex.unlock();
    waiter.join();
    EXPECT_TRUE(thread_acquired.load(std::memory_order_acquire));
}

} // namespace
} // namespace zco
