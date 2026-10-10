#include "runtime/task_queues.h"
#include "runtime/worker.h"

namespace zco {
namespace detail {
void Completion::finish(TaskStatus result, std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex);
    status = result;
    exception = std::move(error);
    if (waiters)
        waiters->complete_all();
}

bool TaskQueues::push(PendingTask task, bool pinned) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool empty = pinned_.empty() && movable_.empty();
    (pinned ? pinned_ : movable_).push_back(std::move(task));
    if (!pinned)
        ++movable_size_;
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
    if (&queue == &movable_)
        --movable_size_;
    --size_;
    return true;
}

bool TaskQueues::steal(PendingTask &task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (movable_.empty())
        return false;
    task = std::move(movable_.back());
    movable_.pop_back();
    --movable_size_;
    --size_;
    return true;
}

void Submission::add_idle(Worker *worker) {
    worker->idle_ = true;
    worker->idle_next_ = idle_first;
    if (idle_first)
        idle_first->idle_previous_ = worker;
    idle_first = worker;
}

void Submission::remove_idle(Worker *worker) {
    if (!worker->idle_)
        return;
    if (worker->idle_previous_)
        worker->idle_previous_->idle_next_ = worker->idle_next_;
    else
        idle_first = worker->idle_next_;
    if (worker->idle_next_)
        worker->idle_next_->idle_previous_ = worker->idle_previous_;
    worker->idle_ = false;
    worker->idle_next_ = worker->idle_previous_ = nullptr;
}

void Submission::wake_idle() {
    if (idle_first) {
        auto *worker = idle_first;
        remove_idle(worker);
        worker->wake();
    }
}

} // namespace detail
} // namespace zco
