#include "timer/timer_manager.h"
#include "zcoroutine_logger.h"
#include <sys/time.h>

namespace zcoroutine {

// 获取当前时间（毫秒）
static uint64_t get_current_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

Timer::ptr TimerManager::add_timer(uint64_t timeout, std::function<void()> callback, bool recurring) {
    auto timer = std::make_shared<Timer>(timeout, callback, recurring);
    
    std::lock_guard<std::mutex> lock(mutex_);
    timers_.insert(timer);
    
    ZCOROUTINE_LOG_DEBUG("TimerManager::add_timer: timeout={}ms, recurring={}, next_time={}, total_timers={}", 
                         timeout, recurring, timer->get_next_time(), timers_.size());
    return timer;
}

int TimerManager::get_next_timeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (timers_.empty()) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: no timers, returning -1");
        return -1;
    }
    
    uint64_t now = get_current_ms();
    auto it = timers_.begin();
    uint64_t next_time = (*it)->get_next_time();
    
    if (next_time <= now) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: timer already expired, returning 0");
        return 0;  // 已经到期
    }
    
    int timeout = static_cast<int>(next_time - now);
    ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout: next_timeout={}ms, total_timers={}", 
                         timeout, timers_.size());
    return timeout;
}

std::vector<std::function<void()>> TimerManager::list_expired_callbacks() {
    std::vector<std::function<void()>> callbacks;
    uint64_t now = get_current_ms();
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t initial_count = timers_.size();
    size_t expired_count = 0;
    size_t cancelled_count = 0;
    
    auto it = timers_.begin();
    while (it != timers_.end()) {
        if ((*it)->get_next_time() > now) {
            break;
        }
        
        auto timer = *it;
        it = timers_.erase(it);
        
        if (!timer->cancelled_) {
            callbacks.push_back([timer]() {
                timer->execute();
            });
            expired_count++;
            
            // 如果是循环定时器，重新插入
            if (timer->is_recurring() && !timer->cancelled_) {
                timers_.insert(timer);
            }
        } else {
            cancelled_count++;
        }
    }
    
    if (expired_count > 0 || cancelled_count > 0) {
        ZCOROUTINE_LOG_DEBUG("TimerManager::list_expired_callbacks: expired={}, cancelled={}, remaining={}, initial={}", 
                             expired_count, cancelled_count, timers_.size(), initial_count);
    }
    
    return callbacks;
}

} // namespace zcoroutine
