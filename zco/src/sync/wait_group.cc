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
    // 1. 检查条件和登记等待者，必须连着做。
    // 否则done()可能在检查后、登记前就把计数降到零，导致丢失唤醒。
    std::unique_lock<std::mutex> lock(mutex_);
    if (!count_)
        return {};
    auto ticket = waiters_.add();

    lock.unlock();

    // 2. 登记之后，真正睡下去（调用 park()）之前，再看一眼
    auto outcome = ticket.wait(deadline);

    // 3. 如果通知刚好发生在“睡下去的动作”中，由调度器补一次检查
    lock.lock();
    waiters_.remove(ticket);
    return Result<void>(wait_error(outcome));
}
} // namespace zco
