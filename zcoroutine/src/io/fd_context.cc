#include "io/fd_context.h"
#include "zcoroutine_logger.h"
#include "util/thread_context.h"
#include "scheduling/scheduler.h"
#include <cstring>

namespace zcoroutine {

FdContext::FdContext(int fd) : fd_(fd), events_(kNone) {
    memset(&read_ctx_, 0, sizeof(read_ctx_));
    memset(&write_ctx_, 0, sizeof(write_ctx_));
    ZCOROUTINE_LOG_DEBUG("FdContext created: fd={}", fd_);
}

int FdContext::add_event(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查事件是否已存在
    if (events_ & event) {
        ZCOROUTINE_LOG_WARN("FdContext::add_event event already exists: fd={}, event={}, current_events={}", 
                            fd_, event, events_);
        return events_;
    }
    
    // 添加事件
    int old_events = events_;
    int new_events = events_ | event;
    events_ = new_events;
    
    ZCOROUTINE_LOG_DEBUG("FdContext::add_event success: fd={}, event={}, old_events={}, new_events={}", 
                         fd_, event, old_events, new_events);
    
    return new_events;
}

int FdContext::del_event(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查事件是否存在
    if (!(events_ & event)) {
        ZCOROUTINE_LOG_DEBUG("FdContext::del_event event not exists: fd={}, event={}, current_events={}", 
                             fd_, event, events_);
        return events_;
    }
    
    // 删除事件
    int old_events = events_;
    int new_events = events_ & ~event;
    events_ = new_events;
    
    // 重置对应的事件上下文
    if (event == kRead) {
        reset_event_context(read_ctx_);
        ZCOROUTINE_LOG_DEBUG("FdContext::del_event READ context reset: fd={}", fd_);
    } else if (event == kWrite) {
        reset_event_context(write_ctx_);
        ZCOROUTINE_LOG_DEBUG("FdContext::del_event WRITE context reset: fd={}", fd_);
    }
    
    ZCOROUTINE_LOG_DEBUG("FdContext::del_event success: fd={}, event={}, old_events={}, new_events={}", 
                         fd_, event, old_events, new_events);
    
    return new_events;
}

int FdContext::cancel_event(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查事件是否存在
    if (!(events_ & event)) {
        ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event event not exists: fd={}, event={}, current_events={}", 
                             fd_, event, events_);
        return events_;
    }
    
    // 获取事件上下文
    EventContext& ctx = get_event_context(event);
    
    // 触发回调或唤醒协程
    if (ctx.callback) {
        ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event triggering callback: fd={}, event={}", fd_, event);
        ctx.callback();
    } else if (ctx.fiber) {
        ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event scheduling fiber: fd={}, event={}, fiber_id={}", 
                             fd_, event, ctx.fiber->id());
        Scheduler* scheduler = Scheduler::get_this();
        if (scheduler) {
            scheduler->schedule(ctx.fiber);
        } else {
            ZCOROUTINE_LOG_WARN("FdContext::cancel_event no scheduler available: fd={}, event={}", fd_, event);
        }
    } else {
        ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event no callback or fiber: fd={}, event={}", fd_, event);
    }
    
    // 删除事件
    int old_events = events_;
    int new_events = events_ & ~event;
    events_ = new_events;
    
    // 重置事件上下文
    reset_event_context(ctx);
    
    ZCOROUTINE_LOG_DEBUG("FdContext::cancel_event success: fd={}, event={}, old_events={}, new_events={}", 
                         fd_, event, old_events, new_events);
    
    return new_events;
}

void FdContext::cancel_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (events_ == kNone) {
        ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all no events to cancel: fd={}", fd_);
        return;
    }
    
    int old_events = events_;
    int read_triggered = 0;
    int write_triggered = 0;
    
    // 取消读事件
    if (events_ & kRead) {
        EventContext& ctx = read_ctx_;
        if (ctx.callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all triggering READ callback: fd={}", fd_);
            ctx.callback();
            read_triggered = 1;
        } else if (ctx.fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all scheduling READ fiber: fd={}, fiber_id={}", 
                                 fd_, ctx.fiber->id());
            Scheduler* scheduler = Scheduler::get_this();
            if (scheduler) {
                scheduler->schedule(ctx.fiber);
            } else {
                ZCOROUTINE_LOG_WARN("FdContext::cancel_all no scheduler for READ fiber: fd={}", fd_);
            }
            read_triggered = 1;
        }
        reset_event_context(ctx);
    }
    
    // 取消写事件
    if (events_ & kWrite) {
        EventContext& ctx = write_ctx_;
        if (ctx.callback) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all triggering WRITE callback: fd={}", fd_);
            ctx.callback();
            write_triggered = 1;
        } else if (ctx.fiber) {
            ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all scheduling WRITE fiber: fd={}, fiber_id={}", 
                                 fd_, ctx.fiber->id());
            Scheduler* scheduler = Scheduler::get_this();
            if (scheduler) {
                scheduler->schedule(ctx.fiber);
            } else {
                ZCOROUTINE_LOG_WARN("FdContext::cancel_all no scheduler for WRITE fiber: fd={}", fd_);
            }
            write_triggered = 1;
        }
        reset_event_context(ctx);
    }
    
    events_ = kNone;
    
    ZCOROUTINE_LOG_DEBUG("FdContext::cancel_all complete: fd={}, old_events={}, read_triggered={}, write_triggered={}", 
                         fd_, old_events, read_triggered, write_triggered);
}

void FdContext::trigger_event(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查事件是否存在
    if (!(events_ & event)) {
        ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event event not registered: fd={}, event={}, current_events={}", 
                             fd_, event, events_);
        return;
    }
    
    // 获取事件上下文
    EventContext& ctx = get_event_context(event);
    
    // 触发回调或唤醒协程
    if (ctx.callback) {
        ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event executing callback: fd={}, event={}", fd_, event);
        ctx.callback();
    } else if (ctx.fiber) {
        ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event scheduling fiber: fd={}, event={}, fiber_id={}", 
                             fd_, event, ctx.fiber->id());
        Scheduler* scheduler = Scheduler::get_this();
        if (scheduler) {
            scheduler->schedule(ctx.fiber);
        } else {
            ZCOROUTINE_LOG_WARN("FdContext::trigger_event no scheduler available: fd={}, event={}", fd_, event);
        }
    } else {
        ZCOROUTINE_LOG_WARN("FdContext::trigger_event no callback or fiber: fd={}, event={}", fd_, event);
    }
    
    // 重置事件上下文（事件被触发后自动删除）
    int old_events = events_;
    events_ = events_ & ~event;
    reset_event_context(ctx);
    
    ZCOROUTINE_LOG_DEBUG("FdContext::trigger_event complete: fd={}, event={}, old_events={}, new_events={}", 
                         fd_, event, old_events, events_);
}

FdContext::EventContext& FdContext::get_event_context(Event event) {
    if (event == kRead) {
        return read_ctx_;
    } else if (event == kWrite) {
        return write_ctx_;
    }
    
    // 不应该到达这里
    ZCOROUTINE_LOG_ERROR("FdContext::get_event_context invalid event: fd={}, event={}", fd_, event);
    return read_ctx_;
}

void FdContext::reset_event_context(EventContext& ctx) {
    bool had_fiber = ctx.fiber != nullptr;
    bool had_callback = ctx.callback != nullptr;
    
    ctx.fiber.reset();
    ctx.callback = nullptr;
    
    if (had_fiber || had_callback) {
        ZCOROUTINE_LOG_DEBUG("FdContext::reset_event_context: fd={}, had_fiber={}, had_callback={}", 
                             fd_, had_fiber, had_callback);
    }
}

}  // namespace zcoroutine
