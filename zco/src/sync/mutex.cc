#include "zco/sync/mutex.h"

namespace zco {
Result<void> Mutex::lock(Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!locked_) {
        locked_ = true;
        return {};
    }
    auto ticket = waiters_.add();
    lock.unlock();
    auto outcome = ticket.wait(deadline);
    lock.lock();
    waiters_.remove(ticket);
    // ready is published only by unlock while granting ownership.
    return Result<void>(wait_error(outcome));
}

bool Mutex::try_lock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (locked_)
        return false;
    locked_ = true;
    return true;
}

void Mutex::unlock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!locked_)
        throw std::logic_error("Unlocking an unlocked Mutex");
    if (!waiters_.complete_one())
        locked_ = false;
}
} // namespace zco
