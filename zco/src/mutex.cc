#include "zco/mutex.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "zco/internal/coroutine_waiter.h"
#include "zco/internal/fiber.h"
#include "zco/internal/runtime_manager.h"
#include "zco/zco_log.h"

namespace zco {

// Mutex 同时支持线程与协程上下文：
// - 线程等待：condition_variable。
// - 协程等待：等待队列 + park/resume。
// 解锁时优先交给协程等待者，避免协程场景被线程竞争长期饿死。

struct Mutex::Impl {
    void cleanup_waiters_locked() { cleanup_waiters_front(&coroutine_waiters); }

    bool locked = false;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<CoroutineWaiterEntry> coroutine_waiters;
};

Mutex::Mutex() : impl_(std::make_shared<Impl>()) {}

Mutex::~Mutex() = default;

void Mutex::lock() const {
    if (!impl_) {
        ZCO_LOG_ERROR("mutex lock failed, impl is null");
        return;
    }

    if (!in_coroutine()) {
        // 线程路径：若已有协程在排队，也要让线程继续等待，避免插队。
        std::unique_lock<std::mutex> lock(impl_->mutex);
        for (;;) {
            impl_->cleanup_waiters_locked();
            if (!impl_->locked && impl_->coroutine_waiters.empty()) {
                impl_->locked = true;
                return;
            }
            impl_->cv.wait(lock);
        }
    }

    Fiber::ptr coroutine = current_fiber_shared();
    if (!coroutine) {
        ZCO_LOG_WARN(
            "mutex lock fallback to thread path, no current coroutine");
        std::unique_lock<std::mutex> lock(impl_->mutex);
        for (;;) {
            impl_->cleanup_waiters_locked();
            if (!impl_->locked && impl_->coroutine_waiters.empty()) {
                impl_->locked = true;
                return;
            }
            impl_->cv.wait(lock);
        }
    }

    std::shared_ptr<std::atomic<bool>> active =
        std::make_shared<std::atomic<bool>>(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cleanup_waiters_locked();
        if (!impl_->locked) {
            impl_->locked = true;
            return;
        }

        impl_->coroutine_waiters.push_back(
            CoroutineWaiterEntry{coroutine, active});
        // 标记当前协程即将进入等待态，后续由 unlock 或超时路径恢复。
        prepare_current_wait();
    }

    (void)park_current();
    active->store(false, std::memory_order_release);
}

void Mutex::unlock() const {
    if (!impl_) {
        ZCO_LOG_WARN("mutex unlock ignored, impl is null");
        return;
    }

    Fiber::ptr resume_target;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->locked) {
            ZCO_LOG_WARN("mutex unlock ignored, mutex is not locked");
            return;
        }

        impl_->cleanup_waiters_locked();
        while (!impl_->coroutine_waiters.empty()) {
            CoroutineWaiterEntry waiter = impl_->coroutine_waiters.front();
            impl_->coroutine_waiters.pop_front();
            resume_target = claim_waiter(&waiter);
            if (resume_target) {
                break;
            }
        }

        if (!resume_target) {
            // 没有可交接的协程等待者时才真正释放锁。
            impl_->locked = false;
        }
    }

    if (resume_target) {
        // 直接交接给协程等待者，保持互斥语义连续。
        resume_fiber(resume_target, false);
        return;
    }

    impl_->cv.notify_one();
}

bool Mutex::try_lock() const {
    if (!impl_) {
        ZCO_LOG_WARN("mutex try_lock failed, impl is null");
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cleanup_waiters_locked();
    if (impl_->locked || !impl_->coroutine_waiters.empty()) {
        return false;
    }

    impl_->locked = true;
    return true;
}

MutexGuard::MutexGuard(const Mutex &mutex) : mutex_(&mutex) { mutex_->lock(); }

MutexGuard::MutexGuard(const Mutex *mutex) : mutex_(mutex) {
    if (mutex_) {
        mutex_->lock();
    }
}

MutexGuard::~MutexGuard() {
    if (mutex_) {
        mutex_->unlock();
    }
}

} // namespace zco
