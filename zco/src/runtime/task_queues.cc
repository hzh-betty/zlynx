#include "runtime/task_queues.h"

namespace zco {
namespace detail {
void Completion::finish(TaskStatus result, std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex);
    status = result;
    exception = std::move(error);
    waiters.complete_all();
}

bool TaskQueues::push(PendingTask task, bool pinned) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool empty = pinned_.empty() && movable_.empty();
    (pinned ? pinned_ : movable_).push_back(std::move(task));
    ++size_;
    return empty;
}

bool TaskQueues::take(PendingTask &task) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &queue = pinned_.empty() ? movable_ : pinned_;
    if (queue.empty())
        return false;
    task = std::move(queue.front());
    queue.pop_front();
    --size_;
    return true;
}

bool TaskQueues::steal(PendingTask &task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (movable_.empty())
        return false;
    task = std::move(movable_.back());
    movable_.pop_back();
    --size_;
    return true;
}

} // namespace detail
} // namespace zco
