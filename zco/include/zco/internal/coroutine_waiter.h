#ifndef ZCO_INTERNAL_COROUTINE_WAITER_H_
#define ZCO_INTERNAL_COROUTINE_WAITER_H_

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "zco/internal/fiber.h"

namespace zco {

struct CoroutineWaiterEntry {
    std::weak_ptr<Fiber> coroutine;
    std::shared_ptr<std::atomic<bool>> active;
};

inline bool is_waiter_entry_valid(const CoroutineWaiterEntry &waiter) {
    if (!waiter.active) {
        return false;
    }
    if (!waiter.active->load(std::memory_order_acquire)) {
        return false;
    }
    return !waiter.coroutine.expired();
}

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