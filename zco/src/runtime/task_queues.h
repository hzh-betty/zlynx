#pragma once
#include "zco/detail/wait_queue.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace zco {
namespace detail {
struct Completion {
    explicit Completion(TaskId identity) : id(identity) {}

    void finish(TaskStatus result, std::exception_ptr error = {});
    TaskId id;
    mutable std::mutex mutex;
    // Constructed under mutex only when a join needs to wait.
    std::optional<WaitQueue> waiters;
    TaskStatus status = TaskStatus::pending;
    std::exception_ptr exception;
};

struct PendingTask {
    Task task;
    std::shared_ptr<Completion> completion;
};

class TaskQueues {
  public:
    bool push(PendingTask task, bool pinned);
    bool take(PendingTask &task);
    bool steal(PendingTask &task);

    size_t size() const { return size_.load(std::memory_order_relaxed); }
    size_t movable_size() const {
        return movable_size_.load(std::memory_order_relaxed);
    }

  private:
    mutable std::mutex mutex_;
    std::deque<PendingTask> pinned_, movable_;
    std::atomic<size_t> size_{0};
    std::atomic<size_t> movable_size_{0};
};
class Worker;

struct Submission {
    // These operations require mutex, as do worker lifetime and admission.
    void add_idle(Worker *);
    void remove_idle(Worker *);
    void wake_idle();
    std::mutex mutex;
    bool accepting = false;
    std::vector<Worker *> workers;
    size_t next_worker = 0;
    Worker *idle_first = nullptr;
};

Result<TaskHandle> submit(const std::shared_ptr<Submission> &, Task,
                          size_t index, bool pinned);
} // namespace detail
} // namespace zco
