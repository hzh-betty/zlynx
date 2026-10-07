#include "zco/internal/io_wait_service.h"

#include "zco/internal/fiber.h"
#include "zco/internal/poller.h"
#include "zco/internal/timer.h"
#include "zco/zco_logger.h"
#include <errno.h>
#include <sys/epoll.h>
#include <utility>

namespace zco {
IoWaitService::IoWaitService(int scheduler_id, TimerQueue &timers,
                             std::unique_ptr<Poller> poller, Resume resume)
    : scheduler_id_(scheduler_id), timers_(timers), poller_(std::move(poller)),
      resume_(std::move(resume)) {}
IoWaitService::~IoWaitService() = default;
bool IoWaitService::start() { return poller_ && poller_->start(); }
void IoWaitService::stop() {
    if (poller_)
        poller_->stop();
}
void IoWaitService::wake() {
    if (poller_)
        poller_->wake();
}
void IoWaitService::poll(int timeout_ms) {
    if (poller_) {
        poller_->wait_events(
            timeout_ms,
            [this](const std::shared_ptr<IoWaiter> &waiter, uint32_t events) {
                handle_ready(waiter, events);
            });
    }
}

bool IoWaitService::wait(const std::shared_ptr<Fiber> &fiber, int fd,
                         uint32_t events, uint32_t milliseconds,
                         const Park &park) {
    // 这里的 waiter 是一次性等待句柄：它同时绑定 fd、感兴趣事件、
    // 当前协程以及可选的超时定时器。后续所有唤醒路径都只会竞争这
    // 一个对象，避免 IO 和超时分别恢复同一协程。
    ZCO_LOG_DEBUG("wait_fd start, sched_id={}, fiber_id={}, fd={}, "
                  "events={}, timeout_ms={}",
                  scheduler_id_, fiber->id(), fd, events, milliseconds);

    std::shared_ptr<IoWaiter> waiter = std::make_shared<IoWaiter>();
    waiter->fd = fd;
    waiter->events = events & (EPOLLIN | EPOLLOUT);
    waiter->fiber = fiber;
    waiter->timer = nullptr;
    waiter->active.store(true, std::memory_order_release);
    waiter->error.store(0, std::memory_order_release);

    if (waiter->events == 0) {
        errno = EINVAL;
        return false;
    }

    fiber->mark_waiting();

    if (!poller_ || !poller_->register_waiter(waiter)) {
        ZCO_LOG_ERROR(
            "epoll add/mod failed, sched_id={}, fd={}, events={}, errno={}",
            scheduler_id_, fd, events, errno);
        // 注册失败时必须把协程状态恢复为 running，否则调用方会认为它
        // 已经进入等待态，但实际上并没有挂到 epoll 上。
        waiter->active.store(false, std::memory_order_release);
        fiber->mark_running();
        return false;
    }

    if (milliseconds != kInfiniteTimeoutMs) {
        // timeout 回调与 IO 回调通过 waiter->active 竞争，只有一个路径
        // 能真正把协程重新投递回调度器。这样即使超时和可读/可写几乎同
        // 时到达，也不会出现重复 resume。
        waiter->timer = timers_.add_timer(milliseconds, [this, waiter]() {
            if (!waiter->active.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            if (poller_) {
                poller_->unregister_waiter(waiter);
            }
            if (Fiber::ptr fiber = waiter->fiber.lock()) {
                ZCO_LOG_DEBUG(
                    "wait_fd timeout, sched_id={}, fd={}, fiber_id={}",
                    scheduler_id_, waiter->fd, fiber->id());
                resume_(fiber, true);
            }
        });
        ZCO_LOG_DEBUG("timer added, sched_id={}, delay_ms={}", scheduler_id_,
                      milliseconds);
        wake();
    }

    const bool ok = park();
    const int waiter_error = waiter->error.load(std::memory_order_acquire);

    // 协程恢复后无论是正常 IO、超时还是 fd 被取消，都要撤销本次 wait
    // 句柄的活跃状态，并解除 epoll 里的兴趣注册，保证下次等待从干净状
    // 态重新开始。
    waiter->active.store(false, std::memory_order_release);
    if (waiter->timer) {
        waiter->timer->cancelled.store(true, std::memory_order_release);
    }
    if (poller_) {
        poller_->unregister_waiter(waiter);
    }

    if (waiter_error != 0) {
        errno = waiter_error;
        return false;
    }

    if (!ok) {
        ZCO_LOG_DEBUG(
            "wait_fd wake failed or timeout, sched_id={}, fd={}, timeout_ms={}",
            scheduler_id_, fd, milliseconds);
    }

    return ok;
}

void IoWaitService::cancel(int fd, int error) {
    if (!poller_ || fd < 0) {
        return;
    }

    // fd 被外部关闭或整体取消时，先让 poller 收集该 fd 上的所有等待者，
    // 再统一把错误码传播给每个协程。这样 IO 路径和超时路径就不会再独
    // 立消费同一个等待句柄。
    std::vector<std::shared_ptr<IoWaiter>> waiters =
        poller_->cancel_fd(fd, error);
    for (size_t i = 0; i < waiters.size(); ++i) {
        const std::shared_ptr<IoWaiter> &waiter = waiters[i];
        if (!waiter ||
            !waiter->active.exchange(false, std::memory_order_acq_rel)) {
            continue;
        }

        if (waiter->timer) {
            waiter->timer->cancelled.store(true, std::memory_order_release);
        }

        if (Fiber::ptr fiber = waiter->fiber.lock()) {
            ZCO_LOG_DEBUG("fd waiter cancelled, sched_id={}, fd={}, "
                          "fiber_id={}, error={}",
                          scheduler_id_, fd, fiber->id(), error);
            resume_(fiber, false);
        }
    }
}

void IoWaitService::handle_ready(const std::shared_ptr<IoWaiter> &waiter,
                                 uint32_t ready_events) {
    (void)ready_events;
    if (!waiter) {
        return;
    }

    if (!waiter->active.exchange(false, std::memory_order_acq_rel)) {
        // 说明已被超时路径或其他路径消费，避免重复恢复。
        return;
    }

    if (waiter->timer) {
        waiter->timer->cancelled.store(true, std::memory_order_release);
    }

    if (Fiber::ptr fiber = waiter->fiber.lock()) {
        ZCO_LOG_DEBUG("io ready resume fiber, sched_id={}, fd={}, "
                      "fiber_id={}, ready_events={}",
                      scheduler_id_, waiter->fd, fiber->id(), ready_events);
        resume_(fiber, false);
    }
}

} // 命名空间 zco
