#include "runtime/worker.h"
#include <algorithm>
#include <limits>

namespace zco {
namespace detail {
thread_local Worker *current_worker = nullptr;

void Inbox::notify(WaitId id) {
    bool empty;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        empty = notifications_.empty();
        notifications_.push_back(id);
    }
    if (empty)
        if (auto reactor = reactor_.lock())
            reactor->wake();
}

std::vector<WaitId> Inbox::drain() {
    std::vector<WaitId> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.swap(notifications_);
    return result;
}

Worker::Worker(size_t index, const RuntimeOptions &options,
               std::weak_ptr<Submission> submission,
               std::shared_ptr<Reactor> reactor)
    : index_(index), steal_cursor_(index + 1),
      submission_(std::move(submission)),
      reactor_(std::move(reactor)), inbox_(std::make_shared<Inbox>(reactor_)),
      stacks_(options), io_waits_(reactor_) {}

Worker::~Worker() {
    if (thread_.joinable()) {
        stop();
        join();
    }
}

void Worker::start(const std::shared_ptr<Startup> &startup) {
    thread_ = std::thread([this, startup] {
        {
            std::unique_lock<std::mutex> lock(startup->mutex);
            ++startup->ready;
            startup->cv.notify_all();
            startup->cv.wait(lock,
                             [&] { return startup->launch || startup->abort; });
            if (startup->abort)
                return;
        }
        current_worker = this;
        run();
        current_worker = nullptr;
    });
}

void Worker::stop() {
    inbox_->stopping = true;
    reactor_->wake();
}

void Worker::join() {
    if (thread_.joinable())
        thread_.join();
}

bool Worker::push(PendingTask task, bool pinned) {
    bool empty = queues_.push(std::move(task), pinned);
    if (empty)
        reactor_->wake();
    return empty;
}

TaskId Worker::current_id() const {
    return current_ ? current_->pending.completion->id : TaskId{};
}

void Worker::entry() {
    auto *worker = current_worker;
    auto *record = worker->current_;
    try {
        record->pending.task();
    } catch (const TaskCanceled &) {
        record->result = TaskStatus::canceled;
    } catch (...) {
        record->exception = std::current_exception();
        record->result = TaskStatus::failed;
    }
    record->phase = Phase::finished;
    record->execution->suspend(worker->caller_);
    std::terminate();
}

void Worker::yield_current() {
    if (inbox_->stopping)
        throw TaskCanceled();
    current_->phase = Phase::ready;
    current_->execution->suspend(caller_);
    if (inbox_->stopping)
        throw TaskCanceled();
}

std::shared_ptr<WaitState> Worker::make_wait() {
    if (!current_)
        throw std::logic_error("Wait without a running task");
    return std::make_shared<WaitState>(
        WaitId{current_id(), ++current_->generation}, inbox_);
}

WaitOutcome Worker::park(const std::shared_ptr<WaitState> &wait,
                         Deadline deadline) {
    if (inbox_->stopping)
        wait->complete(WaitOutcome::canceled);
    if (deadline.expired(Deadline::Clock::now()))
        wait->complete(WaitOutcome::timeout);
    if (wait->completed())
        return wait->outcome();
    if (wait->id().task != current_id())
        throw std::logic_error("Wait belongs to a different task");
    auto timer = timers_.add(deadline, WakeToken(wait));
    current_->wait = wait;
    current_->phase = Phase::waiting;
    try {
        current_->execution->suspend(caller_);
    } catch (...) {
        timers_.cancel(timer);
        current_->wait.reset();
        throw;
    }
    timers_.cancel(timer);
    current_->wait.reset();
    return wait->outcome();
}

Result<void>
Worker::wait_io(const std::shared_ptr<io::detail::Resource> &resource,
                io::Interest interest, Deadline deadline) {
    auto wait = make_wait();
    auto registration = io_waits_.add(wait, resource, interest);
    if (!registration)
        return Result<void>(registration.error());
    auto outcome = park(wait, deadline);
    auto removed = registration.value().finish();
    return Result<void>(outcome == WaitOutcome::ready ? removed.error()
                                                      : wait_error(outcome));
}

bool Worker::take_task(PendingTask &task) {
    if (queues_.take(task))
        return true;
    auto start = steal_cursor_++;
    for (size_t offset = 0; offset < peers_.size(); ++offset) {
        auto *peer = peers_[(start + offset) % peers_.size()];
        if (peer != this && peer->queues_.steal(task))
            return true;
    }
    return false;
}

void Worker::cancel_pending() {
    PendingTask pending;
    while (queues_.take(pending)) {
        pending.task = {};
        pending.completion->finish(TaskStatus::canceled);
    }
}

void Worker::collect_notifications() {
    for (auto id : inbox_->drain()) {
        auto found = tasks_.find(id.task.value);
        if (found == tasks_.end())
            continue;
        auto &record = *found->second;
        if (record.phase == Phase::waiting && record.wait &&
            record.wait->id() == id && record.wait->completed()) {
            record.phase = Phase::ready;
            ready_.push_back(id.task.value);
        }
    }
}

void Worker::resume(Record &record) {
    current_ = &record;
    record.phase = Phase::running;
    try {
        if (!record.execution)
            record.execution.reset(new Continuation(stacks_, entry));
        record.execution->resume(caller_);
        while (record.phase != Phase::finished) {
            try {
                record.execution->save();
                break;
            } catch (...) {
                record.phase = Phase::running;
                record.execution->interrupt(caller_, std::current_exception());
            }
        }
    } catch (...) {
        // Failures before switching do not publish a half-created execution.
        record.result = TaskStatus::failed;
        record.exception = std::current_exception();
        record.phase = Phase::finished;
    }
    current_ = nullptr;
    auto id = record.pending.completion->id.value;
    if (record.phase == Phase::finished) {
        auto completion = record.pending.completion;
        auto result = record.result;
        auto exception = record.exception;
        record.pending.task = {};
        tasks_.erase(
            id); // Release execution resources before publishing completion.
        --live_;
        completion->finish(result, exception);
    } else if (record.phase == Phase::ready ||
               (record.wait && record.wait->completed())) {
        record.phase = Phase::ready;
        ready_.push_back(id);
    }
}

void Worker::run_batch() {
    for (size_t budget = 0; budget < 64; ++budget) {
        if (!ready_.empty() && (inbox_->stopping || budget % 2)) {
            auto id = ready_.front();
            ready_.pop_front();
            auto found = tasks_.find(id);
            if (found != tasks_.end())
                resume(*found->second);
        } else {
            if (inbox_->stopping)
                break;
            PendingTask pending;
            if (!take_task(pending)) {
                if (ready_.empty())
                    break;
                auto id = ready_.front();
                ready_.pop_front();
                resume(*tasks_.at(id));
                continue;
            }
            if (inbox_->stopping) {
                pending.task = {};
                pending.completion->finish(TaskStatus::canceled);
                continue;
            }
            auto completion = pending.completion;
            try {
                auto record = std::unique_ptr<Record>(new Record());
                record->pending = std::move(pending);
                auto id = completion->id.value;
                auto inserted = tasks_.emplace(id, std::move(record));
                ++live_;
                {
                    std::lock_guard<std::mutex> lock(completion->mutex);
                    completion->status = TaskStatus::running;
                }
                resume(*inserted.first->second);
            } catch (...) {
                pending.task = {};
                completion->finish(TaskStatus::failed,
                                   std::current_exception());
            }
        }
    }
}

int Worker::poll_timeout() const {
    int timeout = 0;
    if (ready_.empty() && queues_.size() == 0) {
        auto deadline = timers_.next_deadline();
        timeout = -1;
        if (!deadline.is_infinite()) {
            auto duration = deadline.time() - Deadline::Clock::now();
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                    .count();
            timeout = static_cast<int>(std::max<int64_t>(
                0, std::min<int64_t>(ms + 1, std::numeric_limits<int>::max())));
        }
    }
    return timeout;
}

bool Worker::poll_once(int timeout) {
    bool registered_idle = false;
    if (timeout) {
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);
            // Admission and idle registration share the same lock. Work
            // published before registration must also prevent sleeping.
            bool pending = inbox_->stopping || queues_.size() != 0;
            for (auto *worker : endpoint->workers)
                if (worker->queues_.movable_size()) {
                    pending = true;
                    break;
                }
            if (pending)
                timeout = 0;
            else {
                endpoint->add_idle(this);
                registered_idle = true;
            }
        }
    }
    bool succeeded = true;
    try {
        for (auto event : reactor_->poll(timeout))
            io_waits_.dispatch(event);
    } catch (...) {
        succeeded = false;
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);
            endpoint->accepting = false;
            for (auto *worker : endpoint->workers)
                worker->stop();
        } else
            stop();
    }
    if (registered_idle)
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);
            endpoint->remove_idle(this);
        }
    return succeeded;
}

void Worker::run() {
    bool reactor_failed = false;
    while (true) {
        if (inbox_->stopping) {
            cancel_pending();
            for (auto &entry : tasks_)
                if (entry.second->wait)
                    entry.second->wait->complete(WaitOutcome::canceled);
        }
        timers_.expire(Deadline::Clock::now());
        collect_notifications();
        run_batch();
        if (inbox_->stopping && tasks_.empty())
            break;
        if (!reactor_failed)
            reactor_failed = !poll_once(poll_timeout());
    }
    cancel_pending();
}
} // namespace detail
} // namespace zco
