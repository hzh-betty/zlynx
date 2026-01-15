#include "timer/timer.h"

#include <sys/time.h>

#include "timer/timer_manager.h"
#include "util/zcoroutine_logger.h"

namespace zcoroutine {

// 获取当前时间（毫秒）
static uint64_t get_current_ms() {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

Timer::Timer(uint64_t interval_ms, Callback callback, bool recurring,
             TimerManager *manager)
    : interval_ms_(interval_ms), recurring_(recurring),
      callback_(std::move(callback)), manager_(manager) {
  next_time_ = get_current_ms() + interval_ms_;
  ZCOROUTINE_LOG_DEBUG(
      "Timer created: next_time={}, interval={}ms, recurring={}", next_time_,
      interval_ms_, recurring_);
}

Timer::Timer(uint64_t ms) { next_time_ = get_current_ms() + ms; }

bool Timer::cancel() {
  ZCOROUTINE_LOG_DEBUG("Timer::cancel called");
  std::lock_guard<std::mutex> lock(manager_->mutex_);

  if (!callback_) {
    ZCOROUTINE_LOG_WARN("Timer::cancel failed, already canceled");
    return false;
  }
  callback_ = nullptr;

  if (!erase_timer())
    return false;

  ZCOROUTINE_LOG_DEBUG("Timer::cancel succeeded");
  return true;
}

bool Timer::refresh() {
  ZCOROUTINE_LOG_DEBUG("Timer::refresh called");
  std::lock_guard<std::mutex> lock(manager_->mutex_);

  if (!callback_) {
    ZCOROUTINE_LOG_WARN("Timer::refresh failed, already canceled");
    return false;
  }

  // 找到当前定时器，删除
  if (!erase_timer())
    return false;

  // 重新计算下次触发时间
  next_time_ = get_current_ms() + interval_ms_;
  manager_->timers_.insert(shared_from_this());

  ZCOROUTINE_LOG_DEBUG("Timer::refresh succeeded, new next_time={}",
                       next_time_);
  return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
  ZCOROUTINE_LOG_DEBUG("Timer::reset called, ms={}, from_now={}", ms, from_now);
  std::lock_guard<std::mutex> lock(manager_->mutex_);

  if (!callback_) {
    ZCOROUTINE_LOG_WARN("Timer::reset failed, already canceled");
    return false;
  }

  // 找到当前定时器，删除
  if (!erase_timer())
    return false;

  // 重新计算下次触发时间
  uint64_t now = get_current_ms();
  if (from_now) {
    next_time_ = now + ms;
  } else {
    next_time_ = next_time_ - interval_ms_ + ms;
  }
  interval_ms_ = ms;
  manager_->timers_.insert(shared_from_this());

  ZCOROUTINE_LOG_DEBUG("Timer::reset succeeded, new next_time={}", next_time_);
  return true;
}

bool Timer::erase_timer() {
  auto iter = manager_->timers_.find(shared_from_this());
  if (iter == manager_->timers_.end()) {
    ZCOROUTINE_LOG_WARN(
        "Timer::erase_timer failed, timer not found in manager");
    return false;
  }
  manager_->timers_.erase(iter);
  return true;
}

} // namespace zcoroutine
