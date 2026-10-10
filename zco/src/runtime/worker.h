#pragma once
#include "execution/continuation.h"
#include "io/resource.h"
#include "runtime/task_queues.h"
#include "wait/timer_queue.h"
#include "wait/wait_state.h"
#include <condition_variable>
#include <thread>
#include <unordered_map>

namespace zco {
namespace detail {
struct Startup {
    std::mutex mutex;
    std::condition_variable cv;
    size_t ready = 0;
    bool launch = false, abort = false;
};

class Inbox final : public CompletionEndpoint {
  public:
    explicit Inbox(std::weak_ptr<Reactor> reactor)
        : reactor_(std::move(reactor)) {}

    void notify(WaitId id) override;
    std::vector<WaitId> drain();
    std::atomic<bool> stopping{false};

  private:
    std::mutex mutex_;
    std::vector<WaitId> notifications_;
    std::weak_ptr<Reactor> reactor_;
};

class TaskCanceled : public std::exception {};

class Worker {
  public:
    Worker(size_t index, const RuntimeOptions &, std::weak_ptr<Submission>,
           std::shared_ptr<Reactor>);
    ~Worker();
    void start(const std::shared_ptr<Startup> &);
    void stop();

    void wake() { reactor_->wake(); }

    void join();
    bool push(PendingTask task, bool pinned);
    // Caller holds the submission mutex.
    bool idle() const { return idle_; }

    size_t load() const {
        return queues_.size() + live_.load(std::memory_order_relaxed);
    }

    void set_peers(std::vector<Worker *> peers) { peers_ = std::move(peers); }

    size_t index() const { return index_; }

    std::weak_ptr<Submission> submission() const { return submission_; }

    TaskId current_id() const;
    void yield_current();
    std::shared_ptr<WaitState> make_wait();
    WaitOutcome park(const std::shared_ptr<WaitState> &, Deadline);
    Result<void> wait_io(const std::shared_ptr<io::detail::Resource> &,
                         io::Interest, Deadline);

  private:
    friend struct Submission;
    enum class Phase { running, ready, waiting, finished };

    struct Record {
        PendingTask pending;
        std::unique_ptr<Continuation> execution;
        Phase phase = Phase::ready;
        std::shared_ptr<WaitState> wait;
        uint64_t generation = 0;
        TaskStatus result = TaskStatus::succeeded;
        std::exception_ptr exception;
    };

    static void entry();
    void run();
    bool take_task(PendingTask &);
    void collect_notifications();
    void resume(Record &);
    void cancel_pending();
    size_t index_;
    size_t steal_cursor_;
    std::weak_ptr<Submission> submission_;
    std::vector<Worker *> peers_;
    TaskQueues queues_;
    std::shared_ptr<Reactor> reactor_;
    std::shared_ptr<Inbox> inbox_;
    StackArena stacks_;
    NativeContext caller_;
    std::unordered_map<uint64_t, std::unique_ptr<Record>> tasks_;
    std::deque<uint64_t> ready_;
    std::unordered_map<RegistrationId, std::weak_ptr<WaitState>> io_waits_;
    TimerQueue timers_;
    Record *current_ = nullptr;
    std::atomic<size_t> live_{0};
    Worker *idle_next_ = nullptr, *idle_previous_ = nullptr;
    bool idle_ = false;
    std::thread thread_;
};

extern thread_local Worker *current_worker;
} // namespace detail
} // namespace zco
