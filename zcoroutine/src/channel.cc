#include "channel.h"

#include "scheduler.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {
Channel::Channel(int fd) : fd_(fd) {
  ZCOROUTINE_LOG_DEBUG("Channel created: fd={}", fd_);
}

int Channel::add_event(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);

  int current_events = events_.load(std::memory_order_relaxed);

  // 检查事件是否已存在
  if (current_events & event) {
    ZCOROUTINE_LOG_WARN("Channel::add_event event already exists: fd={}, "
                        "event={}, current_events={}",
                        fd_, event_to_string(event), current_events);
    return current_events;
  }

  // 添加事件
  int old_events = current_events;
  int new_events = current_events | event;
  events_.store(new_events, std::memory_order_relaxed);

  ZCOROUTINE_LOG_DEBUG("Channel::add_event success: fd={}, event={}, "
                       "old_events={}, new_events={}",
                       fd_, event_to_string(event), old_events, new_events);

  return new_events;
}

int Channel::del_event(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);

  int current_events = events_.load(std::memory_order_relaxed);

  // 检查事件是否存在
  if (!(current_events & event)) {
    ZCOROUTINE_LOG_DEBUG("Channel::del_event event not exists: fd={}, "
                         "event={}, current_events={}",
                         fd_, event_to_string(event), current_events);
    return current_events;
  }

  // 删除事件
  int old_events = current_events;
  int new_events = current_events & ~event;
  events_.store(new_events, std::memory_order_relaxed);

  // 重置对应的事件上下文
  if (event == kRead) {
    reset_event_context(read_ctx_);
    ZCOROUTINE_LOG_DEBUG("Channel::del_event READ context reset: fd={}", fd_);
  } else if (event == kWrite) {
    reset_event_context(write_ctx_);
    ZCOROUTINE_LOG_DEBUG("Channel::del_event WRITE context reset: fd={}",
                         fd_);
  }

  ZCOROUTINE_LOG_DEBUG("Channel::del_event success: fd={}, event={}, "
                       "old_events={}, new_events={}",
                       fd_, event_to_string(event), old_events, new_events);

  return new_events;
}

Channel::PopResult Channel::pop_event(Event event) {
  PopResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int current_events = events_.load(std::memory_order_relaxed);
    // 检查事件是否存在
    if (!(current_events & event)) {
      result.remaining_events = current_events;
      result.had_event = false;
      return result;
    }

    // 获取事件上下文并清空对应的事件上下文
    EventContext &ctx = get_event_context(event);
    result.callback = std::move(ctx.callback);
    result.fiber = std::move(ctx.fiber);
    reset_event_context(ctx);

    current_events &= ~event;
    events_.store(current_events, std::memory_order_relaxed);

    result.remaining_events = current_events;
    result.had_event = true;
  }
  return result;
}

int Channel::cancel_event(Event event) {
  int new_events = 0;
  std::function<void()> callback = nullptr;
  Fiber::ptr fiber = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int current_events = events_.load(std::memory_order_relaxed);
    // 检查事件是否存在
    if (!(current_events & event)) {
      ZCOROUTINE_LOG_DEBUG("Channel::cancel_event event not exists: fd={}, "
                           "event={}, current_events={}",
                           fd_, event_to_string(event), current_events);
      return current_events;
    }

    // 获取事件上下文
    EventContext &ctx = get_event_context(event);

    // 先取出callback和fiber，避免在回调中重新注册事件时被后续重置
    callback = std::move(ctx.callback);
    fiber = std::move(ctx.fiber);

    // 删除事件标志并重置事件上下文
    int old_events = current_events;
    new_events = current_events & ~event;
    events_.store(new_events, std::memory_order_relaxed);

    reset_event_context(ctx);

    ZCOROUTINE_LOG_DEBUG("Channel::cancel_event success: fd={}, event={}, "
                         "old_events={}, new_events={}",
                         fd_, event_to_string(event), old_events, new_events);
  }

  auto *scheduler = Scheduler::get_this();
  if (callback) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::cancel_event executing callback: fd={}, event={}", fd_,
        event_to_string(event));
    if (scheduler) {
      scheduler->schedule(std::move(callback));
    } else {
      callback();
    }
  } else if (fiber) {
    ZCOROUTINE_LOG_DEBUG("Channel::cancel_event scheduling fiber: fd={}, "
                         "event={}, fiber_id={}",
                         fd_, event_to_string(event), fiber->id());
    if (scheduler) {
      scheduler->schedule(std::move(fiber));
    } else {
      ZCOROUTINE_LOG_WARN("Channel::cancel_event no owner scheduler: fd={}, "
                          "event={} (fiber left ready)",
                          fd_, event_to_string(event));
    }
  } else {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::cancel_event no callback or fiber: fd={}, event={}", fd_,
        event_to_string(event));
  }

  return new_events;
}

void Channel::cancel_all() {
  // 读/写事件的回调和协程
  std::function<void()> read_callback = nullptr;
  Fiber::ptr read_fiber = nullptr;
  std::function<void()> write_callback = nullptr;
  Fiber::ptr write_fiber = nullptr;
  {
    int write_triggered = 0;
    int read_triggered = 0;
    std::lock_guard<std::mutex> lock(mutex_);

    int current_events = events_.load(std::memory_order_relaxed);

    if (current_events == kNone) {
      ZCOROUTINE_LOG_DEBUG("Channel::cancel_all no events to cancel: fd={}",
                           fd_);
      return;
    }

    int old_events = current_events;

    // 取消读事件
    if (current_events & kRead) {
      EventContext &ctx = read_ctx_;
      read_callback = std::move(ctx.callback);
      read_fiber = std::move(ctx.fiber);
      reset_event_context(ctx);
      if (read_callback || read_fiber) {
        read_triggered = 1;
      }
    }

    // 取消写事件
    if (current_events & kWrite) {
      EventContext &ctx = write_ctx_;
      write_callback = std::move(ctx.callback);
      write_fiber = std::move(ctx.fiber);
      reset_event_context(ctx);
      if (write_callback || write_fiber) {
        write_triggered = 1;
      }
    }

    events_.store(kNone, std::memory_order_relaxed);

    ZCOROUTINE_LOG_DEBUG("Channel::cancel_all complete: fd={}, "
                         "old_events={}, read_triggered={}, write_triggered={}",
                         fd_, old_events, read_triggered, write_triggered);
  }

  auto *scheduler = Scheduler::get_this();

  if (read_callback) {
    ZCOROUTINE_LOG_DEBUG("Channel::cancel_all executing READ callback: fd={}",
                         fd_);
    if (scheduler) {
      scheduler->schedule(std::move(read_callback));
    } else {
      read_callback();
    }
  } else if (read_fiber) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::cancel_all scheduling READ fiber: fd={}, fiber_id={}", fd_,
        read_fiber->id());
    if (scheduler) {
      scheduler->schedule(std::move(read_fiber));
    } else {
      ZCOROUTINE_LOG_WARN("Channel::cancel_all no owner scheduler for READ "
                          "fiber: fd={} (fiber left ready)",
                          fd_);
    }
  }

  if (write_callback) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::cancel_all executing WRITE callback: fd={}", fd_);
    if (scheduler) {
      scheduler->schedule(std::move(write_callback));
    } else {
      write_callback();
    }
  } else if (write_fiber) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::cancel_all scheduling WRITE fiber: fd={}, fiber_id={}", fd_,
        write_fiber->id());
    if (scheduler) {
      scheduler->schedule(std::move(write_fiber));
    } else {
      ZCOROUTINE_LOG_WARN("Channel::cancel_all no owner scheduler for WRITE "
                          "fiber: fd={} (fiber left ready)",
                          fd_);
    }
  }
}

void Channel::trigger_event(Event event) {
  std::function<void()> callback = nullptr;
  Fiber::ptr fiber = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int current_events = events_.load(std::memory_order_relaxed);
    // 检查事件是否存在
    if (!(current_events & event)) {
      ZCOROUTINE_LOG_DEBUG("Channel::trigger_event event not registered: "
                           "fd={}, event={}, current_events={}",
                           fd_, event_to_string(event), current_events);
      return;
    }

    // 获取事件上下文
    EventContext &ctx = get_event_context(event);

    // 先取出callback和fiber，避免回调中重新注册时被后续del_event清空
    callback = std::move(ctx.callback);
    fiber = std::move(ctx.fiber);

    // 清空事件上下文，避免残留 callback/fiber/scheduler
    reset_event_context(ctx);

    // 删除事件标志
    int old_events = current_events;
    current_events &= ~event;
    events_.store(current_events, std::memory_order_relaxed);

    ZCOROUTINE_LOG_DEBUG("Channel::trigger_event deleted event: fd={}, "
                         "event={}, old_events={}, new_events={}",
                         fd_, event_to_string(event), old_events,
                         current_events);
  }

  auto *scheduler = Scheduler::get_this();
  // 触发回调或调度协程
  if (callback) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::trigger_event executing callback: fd={}, event={}", fd_,
        event_to_string(event));
    if (scheduler) {
      scheduler->schedule(std::move(callback));
    } else {
      callback();
    }
  } else if (fiber) {
    ZCOROUTINE_LOG_DEBUG("Channel::trigger_event scheduling fiber: fd={}, "
                         "event={}, fiber_id={}",
                         fd_, event_to_string(event), fiber->id());
    if (scheduler) {
      scheduler->schedule(std::move(fiber));
    } else {
      ZCOROUTINE_LOG_WARN("Channel::trigger_event no owner scheduler: fd={}, "
                          "event={} (fiber left ready)",
                          fd_, event_to_string(event));
    }
  } else {
    ZCOROUTINE_LOG_WARN(
        "Channel::trigger_event no callback or fiber: fd={}, event={}", fd_,
        event_to_string(event));
  }
}

Channel::EventContext &Channel::get_event_context(Event event) {
  if (event == kRead) {
    return read_ctx_;
  } else if (event == kWrite) {
    return write_ctx_;
  }

  // 不应该到达这里
  ZCOROUTINE_LOG_ERROR(
      "Channel::get_event_context invalid event: fd={}, event={}", fd_,
      event_to_string(event));
  return read_ctx_;
}

void Channel::reset_event_context(EventContext &ctx) {
  bool had_fiber = ctx.fiber != nullptr;
  bool had_callback = ctx.callback != nullptr;

  ctx.fiber.reset();
  ctx.callback = nullptr;

  if (had_fiber || had_callback) {
    ZCOROUTINE_LOG_DEBUG(
        "Channel::reset_event_context: fd={}, had_fiber={}, had_callback={}",
        fd_, had_fiber, had_callback);
  }
}
} // namespace zcoroutine
