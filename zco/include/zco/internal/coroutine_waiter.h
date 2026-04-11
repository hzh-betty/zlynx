#ifndef ZCO_INTERNAL_COROUTINE_WAITER_H_
#define ZCO_INTERNAL_COROUTINE_WAITER_H_

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "zco/internal/fiber.h"

namespace zco {

/**
 * @brief 协程等待器条目
 * @details 封装协程和其活跃状态
 */
struct CoroutineWaiterEntry {
    std::weak_ptr<Fiber> coroutine;
    std::shared_ptr<std::atomic<bool>> active;
};

/**
 * @brief 检查等待器条目是否有效
 * @param waiter 等待器条目
 * @return 有效返回 true，否则返回 false
 */
inline bool is_waiter_entry_valid(const CoroutineWaiterEntry &waiter) {
    if (!waiter.active) {
        return false;
    }
    if (!waiter.active->load(std::memory_order_acquire)) {
        return false;
    }
    return !waiter.coroutine.expired();
}

/**
 * @brief 清理无效的等待器条目
 * @param waiters 等待器条目列表
 */
inline void cleanup_waiters(std::vector<CoroutineWaiterEntry> *waiters) {
    if (!waiters) {
        return;
    }

    waiters->erase(std::remove_if(waiters->begin(), waiters->end(),
                                  [](const CoroutineWaiterEntry &waiter) {
                                      return !is_waiter_entry_valid(waiter);
                                  }),
                   waiters->end());
}

/**
 * @brief 从前面清理无效的等待器条目，直到遇到第一个有效条目
 * @param waiters 等待器条目列表
 */
inline void cleanup_waiters_front(std::deque<CoroutineWaiterEntry> *waiters) {
    if (!waiters) {
        return;
    }

    while (!waiters->empty()) {
        if (is_waiter_entry_valid(waiters->front())) {
            return;
        }
        waiters->pop_front();
    }
}

/**
 * @brief 申请等待器
 * @param waiter 等待器条目
 * @return 协程指针
 */

inline Fiber::ptr claim_waiter(CoroutineWaiterEntry *waiter) {
    if (!waiter || !waiter->active) {
        return nullptr;
    }

    if (!waiter->active->exchange(false, std::memory_order_acq_rel)) {
        return nullptr;
    }

    return waiter->coroutine.lock();
}

} // namespace zco

#endif // ZCO_INTERNAL_COROUTINE_WAITER_H_