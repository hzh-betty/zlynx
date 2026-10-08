#pragma once
#include "zco/deadline.h"
#include "zco/result.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>

namespace zco {
namespace detail {
struct Completion;
struct Submission;
} // namespace detail

using Task = std::function<void()>;

struct TaskId {
    uint64_t value = 0;

    explicit operator bool() const { return value != 0; }
};

inline bool operator==(TaskId a, TaskId b) { return a.value == b.value; }

inline bool operator!=(TaskId a, TaskId b) { return !(a == b); }
enum class StackModel { kShared, kIndependent };

struct RuntimeOptions {
    size_t worker_count = 0; // 0 selects hardware concurrency.
    size_t stack_size = 256 * 1024;
    size_t shared_stack_count = 8;
    StackModel stack_model = StackModel::kShared;
};
enum class TaskStatus { pending, running, succeeded, failed, canceled };

class TaskHandle {
  public:
    TaskHandle() = default;
    TaskId id() const;
    TaskStatus status() const;
    // join observes task exceptions, and returns cancellation/timeout errors.
    // Unobserved exceptions remain in the completion state; no implicit
    // logging.
    Result<void> join(Deadline deadline = {}) const;
    std::exception_ptr exception() const;

    explicit operator bool() const { return bool(state_); }

    explicit TaskHandle(std::shared_ptr<detail::Completion> state);

  private:
    std::shared_ptr<detail::Completion> state_;
};

class Executor {
  public:
    Executor() = default;
    Result<TaskHandle> spawn(Task task) const;
    bool valid() const;

    size_t index() const { return index_; }

    bool same_runtime(const Executor &other) const;

  private:
    friend class Runtime;
    friend Executor current_executor();

    Executor(std::weak_ptr<detail::Submission> endpoint, size_t index)
        : endpoint_(std::move(endpoint)), index_(index) {}

    std::weak_ptr<detail::Submission> endpoint_;
    size_t index_ = 0;
};

class Runtime {
  public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Result<TaskHandle> spawn(Task task);
    Executor executor(size_t index) const;
    size_t worker_count() const;
    const RuntimeOptions &options() const;
    void request_stop();
    // Must be called outside this runtime's workers. Destruction stops and
    // joins. User code must cooperate at yield/wait points; active stacks are
    // never freed.
    void join();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zco
