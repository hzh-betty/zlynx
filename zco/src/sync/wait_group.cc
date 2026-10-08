#include "zco/sync/wait_group.h"
#include <limits>

namespace zco {
void WaitGroup::add(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count > std::numeric_limits<size_t>::max() - count_)
        throw std::overflow_error("WaitGroup overflow");
    count_ += count;
}

void WaitGroup::done() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!count_)
        throw std::logic_error("WaitGroup underflow");
    if (--count_ == 0)
        waiters_.complete_all();
}

Result<void> WaitGroup::wait(Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!count_)
        return {};
    auto ticket = waiters_.add();
    lock.unlock();
    auto outcome = ticket.wait(deadline);
    lock.lock();
    waiters_.remove(ticket);
    return Result<void>(wait_error(outcome));
}
} // namespace zco
