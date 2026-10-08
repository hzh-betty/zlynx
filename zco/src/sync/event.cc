#include "zco/sync/event.h"

namespace zco {
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
