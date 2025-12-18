#include "util/thread_context.h"

namespace zcoroutine {

// 线程本地变量，存储当前线程的上下文
thread_local ThreadContext* t_thread_context = nullptr;

ThreadContext* ThreadContext::GetCurrent() {
    if (!t_thread_context) {
        t_thread_context = new ThreadContext();
    }
    return t_thread_context;
}

void ThreadContext::SetCurrentFiber(Fiber* fiber) {
    GetCurrent()->current_fiber_ = fiber;
}

Fiber* ThreadContext::GetCurrentFiber() {
    return GetCurrent()->current_fiber_;
}

void ThreadContext::SetSchedulerFiber(Fiber* fiber) {
    GetCurrent()->scheduler_fiber_ = fiber;
}

Fiber* ThreadContext::GetSchedulerFiber() {
    return GetCurrent()->scheduler_fiber_;
}

void ThreadContext::SetScheduler(Scheduler* scheduler) {
    GetCurrent()->scheduler_ = scheduler;
}

Scheduler* ThreadContext::GetScheduler() {
    return GetCurrent()->scheduler_;
}

} // namespace zcoroutine
