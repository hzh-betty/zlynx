#include "timer/timer.h"
#include "zcoroutine_logger.h"
#include <sys/time.h>

namespace zcoroutine {

// 获取当前时间（毫秒）
static uint64_t get_current_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

Timer::Timer(uint64_t timeout, std::function<void()> callback, bool recurring)
    : interval_(timeout)
    , recurring_(recurring)
    , callback_(std::move(callback)) {
    
    next_time_ = get_current_ms() + timeout;
    
    ZCOROUTINE_LOG_DEBUG("Timer created: next_time={}, interval={}, recurring={}", 
                         next_time_, interval_, recurring_);
}

void Timer::cancel() {
    cancelled_ = true;
    callback_ = nullptr;
    ZCOROUTINE_LOG_DEBUG("Timer cancelled: interval={}", interval_);
}

void Timer::refresh() {
    uint64_t old_next_time = next_time_;
    next_time_ = get_current_ms() + interval_;
    ZCOROUTINE_LOG_DEBUG("Timer refreshed: old_next_time={}, new_next_time={}, interval={}", 
                         old_next_time, next_time_, interval_);
}

void Timer::reset(uint64_t timeout) {
    uint64_t old_interval = interval_;
    interval_ = timeout;
    next_time_ = get_current_ms() + timeout;
    ZCOROUTINE_LOG_DEBUG("Timer reset: old_interval={}, new_interval={}, next_time={}", 
                         old_interval, interval_, next_time_);
}

void Timer::execute() {
    if (cancelled_ || !callback_) {
        ZCOROUTINE_LOG_DEBUG("Timer skipped execution: cancelled={}, has_callback={}", 
                             cancelled_, callback_ != nullptr);
        return;
    }
    
    ZCOROUTINE_LOG_DEBUG("Timer executing: next_time={}, interval={}, recurring={}", 
                         next_time_, interval_, recurring_);
    callback_();
    
    // 如果是循环定时器，重新计算下次触发时间
    if (recurring_) {
        next_time_ += interval_;
        ZCOROUTINE_LOG_DEBUG("Timer rescheduled: new_next_time={}", next_time_);
    }
}

} // namespace zcoroutine
