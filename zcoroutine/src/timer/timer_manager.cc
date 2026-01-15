#include "timer/timer_manager.h"

#include <sys/time.h>

#include "util/zcoroutine_logger.h"

namespace zcoroutine {
// 获取当前时间（毫秒）
static uint64_t get_current_ms() {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// TimerManager 析构不需要执行未完成的定时器回调，
// 因为这样可能会访问已经销毁栈上的对象
TimerManager::~TimerManager() {
  std::lock_guard<std::mutex> lock(mutex_);
  timers_.clear();
  ZCOROUTINE_LOG_DEBUG("TimerManager destroyed");
}

Timer::ptr TimerManager::add_timer(uint64_t timeout,
                                   std::function<void()> callback,
                                   bool recurring) {
  ZCOROUTINE_LOG_DEBUG(
      "TimerManager::add_timer called, timeout={}ms, recurring={}", timeout,
      recurring);
  auto timer =
      std::make_shared<Timer>(timeout, std::move(callback), recurring, this);
  insert_timer(timer);
  ZCOROUTINE_LOG_DEBUG("TimerManager::add_timer succeeded, total_timers={}",
                       timers_.size());
  return timer;
}

Timer::ptr TimerManager::add_condition_timer(uint64_t timeout,
                                             std::function<void()> callback,
                                             std::weak_ptr<void> weak_cond,
                                             bool recurring) {
  ZCOROUTINE_LOG_DEBUG(
      "TimerManager::add_condition_timer called, timeout={}ms, recurring={}",
      timeout, recurring);

  // 包装回调函数，添加条件检查
  auto wrapper_callback = [weak = std::move(weak_cond),
                           callback = std::move(callback)]() {
    if (auto ptr = weak.lock()) {
      callback();
    }
  };

  Timer::ptr timer(
      new Timer(timeout, std::move(wrapper_callback), recurring, this));
  insert_timer(timer);
  ZCOROUTINE_LOG_DEBUG("TimerManager::add_condition_timer succeeded");
  return timer;
}

int TimerManager::get_next_timeout() {
  is_ticked_ = false;

  std::lock_guard<std::mutex> lock(mutex_);
  ZCOROUTINE_LOG_DEBUG("TimerManager::get_next_timeout called");

  if (timers_.empty()) {
    ZCOROUTINE_LOG_DEBUG("No timers available");
    return -1;
  }

  uint64_t now = get_current_ms();
  auto next_timer = *timers_.begin();
  if (next_timer->next_time_ <= now) {
    return 0; // 已经到期
  }

  int timeout = static_cast<int>(next_timer->next_time_ - now);
  ZCOROUTINE_LOG_DEBUG("Next timer expires in {} ms", timeout);
  return timeout;
}

std::vector<std::function<void()>> TimerManager::list_expired_callbacks() {
  ZCOROUTINE_LOG_DEBUG("TimerManager::list_expired_callbacks called");
  uint64_t now = get_current_ms();
  std::vector<std::function<void()>> callbacks;

  std::lock_guard<std::mutex> lock(mutex_);
  bool rollback = detect_clock_rollback();

  // 收集到期的定时器回调函数(包括回滞的情况)
  while (!timers_.empty() &&
         (rollback || (*timers_.begin())->next_time_ <= now)) {
    auto timer = *timers_.begin();
    timers_.erase(timers_.begin());

    if (timer->callback_) {
      callbacks.push_back(timer->callback_);
    }

    // 如果是循环定时器，重新计算下次触发时间并插入集合
    if (timer->recurring_ && timer->callback_) {
      timer->next_time_ = now + timer->interval_ms_;
      timers_.insert(timer);
    } else {
      timer->callback_ = nullptr;
    }
  }

  ZCOROUTINE_LOG_DEBUG(
      "TimerManager::list_expired_callbacks completed, count={}",
      callbacks.size());
  return callbacks;
}

bool TimerManager::has_timer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !timers_.empty();
}

void TimerManager::insert_timer(Timer::ptr timer) {
  bool at_front = false;

  // 插入定时器到集合中
  {
    std::lock_guard<std::mutex> lock(mutex_);
    at_front =
        timers_.empty() || (timer->next_time_ < (*timers_.begin())->next_time_);
    ZCOROUTINE_LOG_DEBUG("Inserting timer, at_front={}", at_front);
    timers_.insert(std::move(timer));
  }

  // 如果插入的定时器在最前面，调用回调通知
  if (at_front && !is_ticked_ && on_timer_inserted_callback_) {
    is_ticked_ = true;
    on_timer_inserted_callback_();
    ZCOROUTINE_LOG_DEBUG("Timer inserted at front, callback notified");
  }
}

bool TimerManager::detect_clock_rollback() {
  bool rollback = false;
  uint64_t now = get_current_ms();
  // 如果当前时间比上次检测时间小超1小时以上，认为时钟回播
  if (now < last_time_ && (last_time_ - now) > 60 * 60 * 1000) {
    rollback = true;
    ZCOROUTINE_LOG_WARN("Clock rollback detected, now={}, last_time={}", now,
                        last_time_);
  }
  last_time_ = now;
  return rollback;
}
} // namespace zcoroutine
