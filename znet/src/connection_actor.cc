#include "znet/internal/connection_actor.h"

#include "zco/sched.h"
#include <errno.h>
#include <utility>

namespace znet {
namespace detail {
ConnectionActor::ConnectionActor(zco::Scheduler *scheduler, Handler handler,
                                 KeepAlive keep_alive)
    : handler_(std::move(handler)), keep_alive_(std::move(keep_alive)),
      scheduler_(scheduler) {
    if (!scheduler_) {
        scheduler_ = zco::next_sched();
        if (!scheduler_)
            scheduler_ = zco::main_sched();
    }
    if (scheduler_)
        sched_id_ = scheduler_->id();
}

ssize_t ConnectionActor::dispatch(const EventPtr &event) {
    if (!event) {
        errno = EINVAL;
        return -1;
    }

    // actor 尚未运行时，需要决定谁来启动 drain。
    bool should_launch_worker = false;

    // 当前就在目标协程调度器上时，可直接 inline 处理，
    // 避免“再派发一个协程”带来的额外调度开销。
    bool run_inline = false;

    // 重入场景：若当前已在 actor 执行协程内，再次 dispatch 时必须直接执行，
    // 否则会出现“自己等待自己”的死锁。注意不能用线程 id 判断：同一
    // 调度线程会轮流运行多个协程，只有同一个协程才是真正的 actor 重入。
    bool reentrant = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (running_ && coroutine_ != nullptr &&
            coroutine_ == zco::current_coroutine()) {
            reentrant = true;
        } else {
            mailbox_.push_back(event);
            if (!running_) {
                running_ = true;
                if (zco::in_coroutine() &&
                    (sched_id_ < 0 || zco::sched_id() == sched_id_)) {
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
        if (scheduler_) {
            scheduler_->go([this, owner]() { drain(); });
        } else {
            zco::go([this, owner]() { drain(); });
        }
    }

    event->completion.wait();

    if (event->error != 0) {
        errno = event->error;
    }
    return event->result;
}

bool ConnectionActor::try_begin_inline() {
    if (!zco::in_coroutine()) {
        return false;
    }

    if (sched_id_ >= 0 && zco::sched_id() != sched_id_) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (running_) {
        return false;
    }

    running_ = true;
    coroutine_ = zco::current_coroutine();
    return true;
}

void ConnectionActor::finish_inline() { drain(); }

void ConnectionActor::drain() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        coroutine_ = zco::current_coroutine();
    }

    while (true) {
        EventPtr event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (mailbox_.empty()) {
                // 邮箱耗尽后释放 running_，下一个事件可重新拉起 worker。
                running_ = false;
                coroutine_ = nullptr;
                return;
            }
            event = mailbox_.front();
            mailbox_.pop_front();
        }

        handler_(event);
        event->completion.signal();
    }
}

} // 命名空间 detail
} // 命名空间 znet
