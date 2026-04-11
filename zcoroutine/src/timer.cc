#include "zcoroutine/internal/timer.h"

#include <algorithm>
#include <chrono>

#include "zcoroutine/log.h"

namespace zcoroutine {

// TimerQueue 提供“按截止时间触发回调”的最小能力：
// - add_timer: 录入 deadline + callback。
// - process_due: 执行到期回调。
// - next_timeout_ms: 告诉 epoll_wait 下一次最多可阻塞多久。

TimerQueue::TimerQueue() : mutex_(), timers_(), sequence_(0) {}

std::shared_ptr<TimerToken>
TimerQueue::add_timer(uint32_t milliseconds, std::function<void()> callback) {
    std::shared_ptr<TimerToken> token = std::make_shared<TimerToken>();
    const uint64_t deadline_ms = now_ms() + milliseconds;
    const uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
    // seq 用于在相同 deadline 下提供稳定顺序，避免无序抖动。

    {
        std::lock_guard<std::mutex> lock(mutex_);
        timers_.push(TimerEntry{deadline_ms, seq, token, std::move(callback)});
        ZCOROUTINE_LOG_DEBUG("timer queued, delay_ms={}, queue_size={}",
                             milliseconds, timers_.size());
    }

    return token;
}

void TimerQueue::process_due() {
    // 到期定时器在调度线程串行执行，避免跨线程回调竞态。
    while (true) {
        TimerEntry entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (timers_.empty()) {
                return;
            }
            if (timers_.top().deadline_ms > now_ms()) {
                return;
            }
            entry = timers_.top();
            timers_.pop();
        }

        if (entry.token &&
            entry.token->cancelled.load(std::memory_order_acquire)) {
            continue;
        }

        if (entry.callback) {
            ZCOROUTINE_LOG_DEBUG("timer fired, deadline_ms={}",
                                 entry.deadline_ms);
            entry.callback();
        }
    }
}

int TimerQueue::next_timeout_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) {
        // 空队列给一个保守上限，避免 epoll 长时间无限阻塞。
        return 1000;
    }

    const uint64_t now = now_ms();
    const uint64_t deadline = timers_.top().deadline_ms;
    if (deadline <= now) {
        return 0;
    }

    const uint64_t delta = deadline - now;
    return delta > 1000 ? 1000 : static_cast<int>(delta);
}

uint64_t now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace zcoroutine
