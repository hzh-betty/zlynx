#include "zcoroutine/event.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

#include "zcoroutine/internal/coroutine_waiter.h"
#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/runtime_manager.h"
#include "zcoroutine/log.h"

namespace zcoroutine {

// Event 的实现目标：
// 1) 在普通线程中使用条件变量阻塞/唤醒。
// 2) 在协程中挂起/恢复，避免阻塞调度线程。
// 3) 统一支持 manual_reset 与 auto_reset 语义。

struct Event::Impl {
    /**
     * @brief 构造事件状态。
     * @param manual_reset_arg 手动复位标识。
     * @param signaled_arg 初始触发状态。
     */
    Impl(bool manual_reset_arg, bool signaled_arg)
        : manual_reset(manual_reset_arg), signaled(signaled_arg), mutex(), cv(),
          waiters() {}

    /**
     * @brief 清理无效等待节点。
     * @return 无返回值。
     */
    void cleanup_waiters() { zcoroutine::cleanup_waiters(&waiters); }

    bool manual_reset;
    bool signaled;
    // 线程上下文下的同步原语。
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    // 协程上下文下的等待列表。
    mutable std::vector<CoroutineWaiterEntry> waiters;
};

Event::Event(bool manual_reset, bool signaled)
    : impl_(std::make_shared<Impl>(manual_reset, signaled)) {}

Event::~Event() = default;

Event::Event(const Event &other) = default;

Event::Event(Event &&other) noexcept = default;

bool Event::wait(uint32_t milliseconds) const {
    if (!impl_) {
        ZCOROUTINE_LOG_ERROR("event wait failed, impl is null");
        return false;
    }

    if (!in_coroutine()) {
        // 线程模式：完全由条件变量完成阻塞与超时。
        std::unique_lock<std::mutex> lock(impl_->mutex);
        if (impl_->signaled) {
            if (!impl_->manual_reset) {
                impl_->signaled = false;
            }
            return true;
        }

        if (milliseconds == kInfiniteTimeoutMs) {
            impl_->cv.wait(lock, [this]() { return impl_->signaled; });
            if (!impl_->manual_reset) {
                impl_->signaled = false;
            }
            return true;
        }

        const bool ok =
            impl_->cv.wait_for(lock, std::chrono::milliseconds(milliseconds),
                               [this]() { return impl_->signaled; });
        if (!ok) {
            ZCOROUTINE_LOG_DEBUG(
                "event wait timeout in thread context, timeout_ms={}",
                milliseconds);
            return false;
        }

        if (!impl_->manual_reset) {
            impl_->signaled = false;
        }
        return true;
    }

    // 协程模式：将当前协程登记到 waiters，随后挂起，等待 signal/notify_all
    // 或超时。
    Fiber::ptr coroutine = current_fiber_shared();
    if (!coroutine) {
        ZCOROUTINE_LOG_WARN(
            "event wait in coroutine context but no current coroutine");
        return false;
    }

    std::shared_ptr<std::atomic<bool>> active =
        std::make_shared<std::atomic<bool>>(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cleanup_waiters();

        if (impl_->signaled) {
            if (!impl_->manual_reset) {
                impl_->signaled = false;
            }
            return true;
        }

        impl_->waiters.push_back(CoroutineWaiterEntry{coroutine, active});
    }

    // active 作为“等待资格令牌”：
    // - signal/notify_all 里 exchange(false) 成功者负责唤醒。
    // - wait 超时后也会把 active 置 false，避免后续重复恢复同一协程。
    prepare_current_wait();
    // park_current/park_current_for 在调度器中切换到其它协程运行。
    const bool ok = milliseconds == kInfiniteTimeoutMs
                        ? park_current()
                        : park_current_for(milliseconds);
    if (!ok) {
        active->store(false, std::memory_order_release);
        ZCOROUTINE_LOG_DEBUG(
            "event wait timeout in coroutine context, timeout_ms={}",
            milliseconds);
    }
    return ok;
}

void Event::signal() const {
    if (!impl_) {
        ZCOROUTINE_LOG_WARN("event signal ignored, impl is null");
        return;
    }

    std::vector<Fiber::ptr> resume_list;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cleanup_waiters();

        if (impl_->manual_reset) {
            // manual_reset：保持 signaled=true，并尝试唤醒所有等待协程。
            impl_->signaled = true;
            for (size_t i = 0; i < impl_->waiters.size(); ++i) {
                CoroutineWaiterEntry &waiter = impl_->waiters[i];
                Fiber::ptr coroutine = claim_waiter(&waiter);
                if (coroutine) {
                    resume_list.push_back(coroutine);
                }
            }
        } else {
            // auto_reset：只唤醒一个等待协程；若当前无等待者，则缓存 signaled
            // 状态。 缓存的 signaled 会在下一次 wait 进入时被消费。
            bool resumed = false;
            for (size_t i = 0; i < impl_->waiters.size(); ++i) {
                CoroutineWaiterEntry &waiter = impl_->waiters[i];
                Fiber::ptr coroutine = claim_waiter(&waiter);
                if (coroutine) {
                    resume_list.push_back(coroutine);
                    resumed = true;
                    break;
                }
            }

            if (!resumed) {
                impl_->signaled = true;
            }
        }

        impl_->cleanup_waiters();
    }

    impl_->cv.notify_all();

    ZCOROUTINE_LOG_DEBUG("event signal wake coroutine count={}",
                         resume_list.size());
    // 协程唤醒放在锁外执行，避免在锁内触发复杂调度链路。
    for (size_t i = 0; i < resume_list.size(); ++i) {
        resume_fiber(resume_list[i], false);
    }
}

void Event::reset() const {
    if (!impl_) {
        ZCOROUTINE_LOG_WARN("event reset ignored, impl is null");
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->signaled = false;
    ZCOROUTINE_LOG_DEBUG("event reset done");
}

void Event::notify_all() const {
    if (!impl_) {
        ZCOROUTINE_LOG_WARN("event notify_all ignored, impl is null");
        return;
    }

    std::vector<Fiber::ptr> resume_list;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cleanup_waiters();

        for (size_t i = 0; i < impl_->waiters.size(); ++i) {
            CoroutineWaiterEntry &waiter = impl_->waiters[i];
            Fiber::ptr coroutine = claim_waiter(&waiter);
            if (coroutine) {
                resume_list.push_back(coroutine);
            }
        }

        impl_->signaled = impl_->manual_reset;
        // auto_reset 下 notify_all 后不保留 signaled；manual_reset
        // 下保持触发态。
        impl_->cleanup_waiters();
    }

    impl_->cv.notify_all();

    ZCOROUTINE_LOG_DEBUG("event notify_all wake coroutine count={}",
                         resume_list.size());
    // notify_all 强制唤醒协程等待队列中所有活跃节点。
    for (size_t i = 0; i < resume_list.size(); ++i) {
        resume_fiber(resume_list[i], false);
    }
}

} // namespace zcoroutine
