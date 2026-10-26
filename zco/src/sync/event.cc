#include "zco/sync/event.h"

namespace zco {
/*
    协程 A                                  协程 B
    检查事件，发现尚未触发
    add()：登记等待，拿到 ticket
    释放事件锁
    ticket.wait()：挂起
                                            Event::signal()
                                            ↓
                                            队列 complete_one()
                                            ↓
                                            WaitState::complete(ready)
                                            ↓
                                            通知 A 所属的 Worker
    Worker 调度 A 恢复
    ticket.wait() 返回 ready
    清理队列登记
*/
Result<void> Event::wait(Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (signaled_) {
        if (!manual_reset_)
            signaled_ = false;
        return {};
    }
    auto ticket = waiters_.add();
    lock.unlock();
    auto outcome = ticket.wait(deadline);
    lock.lock();
    waiters_.remove(ticket);
    return Result<void>(wait_error(outcome));
}

void Event::signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (manual_reset_) {
        signaled_ = true;
        waiters_.complete_all();
    } else if (!waiters_.complete_one())
        signaled_ = true;
}

void Event::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = false;
}
} // namespace zco
