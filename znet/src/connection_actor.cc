#include "znet/internal/connection_actor.h"

#include "zco/coroutine.h"
#include <errno.h>
#include <utility>

namespace znet {
namespace detail {
ConnectionActor::ConnectionActor(zco::Executor scheduler, Handler handler,
                                 KeepAlive keep_alive)
    : handler_(std::move(handler)), keep_alive_(std::move(keep_alive)),
      scheduler_(scheduler) {
    if (!scheduler_.valid())
        throw std::invalid_argument("ConnectionActor requires a live executor");
    sched_id_ = static_cast<int>(scheduler_.index());
}

ssize_t ConnectionActor::dispatch(const EventPtr &event) {
    if (!event) {
        errno = EINVAL;
        return -1;
    }

    // actor 尚未运行时，需要决定谁来启动 drain。
    bool should_launch_worker = false;
    uint64_t generation = 0;

    // 当前就在目标协程调度器上时，可直接 inline 处理，
    // 避免“再派发一个协程”带来的额外调度开销。
    bool run_inline = false;

    // 重入场景：若当前已在 actor 执行协程内，再次 dispatch 时必须直接执行，
    // 否则会出现“自己等待自己”的死锁。注意不能用线程 id 判断：同一
    // 调度线程会轮流运行多个协程，只有同一个协程才是真正的 actor 重入。
    bool reentrant = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (running_ && bool(coroutine_) &&
            coroutine_ == zco::current_task_id()) {
            reentrant = true;
        } else {
            mailbox_.push_back(event);
            if (!running_) {
                running_ = true;
                generation = ++generation_;
                if (zco::in_coroutine() &&
                    (zco::current_executor().same_runtime(scheduler_) &&
                     static_cast<int>(zco::current_executor().index()) ==
                         sched_id_)) {
                    run_inline = true;
                } else {
                    should_launch_worker = true;
                }
            }
        }
    }

    if (reentrant) {
        handler_(event);
        if (event->error != 0) {
            errno = event->error;
        }
        return event->result;
    }

    if (run_inline) {
        drain();
    } else if (should_launch_worker) {
        std::shared_ptr<void> owner = keep_alive_();
        auto lifetime = std::shared_ptr<void>(
            owner.get(),
            [this, owner, generation](void *) { cancel_pending(generation); });
        auto submitted = scheduler_.spawn([this, lifetime]() { drain(); });
        if (!submitted) {
            std::unique_lock<std::mutex> lock(mutex_);
            running_ = false;
            for (auto &pending : mailbox_) {
                pending->error = submitted.error().value();
                pending->result = -1;
                pending->completion.signal();
            }
            mailbox_.clear();
        }
    }

    auto completed = event->completion.wait();
    if (!completed) {
        errno = completed.error().value();
        return -1;
    }

    if (event->exception)
        std::rethrow_exception(event->exception);
    if (event->error != 0) {
        errno = event->error;
    }
    return event->result;
}

bool ConnectionActor::try_begin_inline() {
    if (!zco::in_coroutine()) {
        return false;
    }

    if (!zco::current_executor().same_runtime(scheduler_) ||
        static_cast<int>(zco::current_executor().index()) != sched_id_) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (running_) {
        return false;
    }

    running_ = true;
    coroutine_ = zco::current_task_id();
    return true;
}

void ConnectionActor::finish_inline() { drain(); }

void ConnectionActor::drain() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        coroutine_ = zco::current_task_id();
    }

    while (true) {
        EventPtr event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (mailbox_.empty()) {
                // 邮箱耗尽后释放 running_，下一个事件可重新拉起 worker。
                running_ = false;
                coroutine_ = {};
                return;
            }
            event = mailbox_.front();
            mailbox_.pop_front();
        }

        try {
            handler_(event);
        } catch (...) {
            event->exception = std::current_exception();
            event->result = -1;
            event->error = EIO;
        }
        event->completion.signal();
    }
}

void ConnectionActor::cancel_pending(uint64_t generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_ || generation != generation_)
        return;
    running_ = false;
    coroutine_ = {};
    for (auto &event : mailbox_) {
        event->result = -1;
        event->error = ECANCELED;
        event->completion.signal();
    }
    mailbox_.clear();
}

} // 命名空间 detail
} // 命名空间 znet
