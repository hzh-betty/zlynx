#include "zco/runtime.h"
#include "io/linux/epoll_reactor.h"
#include "runtime/worker.h"
#include "zco/coroutine.h"
#include <atomic>
#include <limits>

namespace zco {
namespace detail {
namespace {
std::atomic<uint64_t> next_task{0};
}

Result<TaskHandle> submit(const std::shared_ptr<Submission> &endpoint,
                          Task task, size_t index, bool pinned) {
    if (!task)
        return Result<TaskHandle>(
            std::make_error_code(std::errc::invalid_argument));
    if (!endpoint)
        return Result<TaskHandle>(wait_error(WaitOutcome::canceled));
    std::lock_guard<std::mutex> lock(endpoint->mutex);
    if (!endpoint->accepting)
        return Result<TaskHandle>(wait_error(WaitOutcome::canceled));
    auto &workers = endpoint->workers;
    if (pinned && index >= workers.size())
        return Result<TaskHandle>(
            std::make_error_code(std::errc::invalid_argument));
    if (!pinned) {
        index = endpoint->next_worker;
        endpoint->next_worker = (index + 1) % workers.size();
        // Rotate the first candidate and compare one separated peer. Stealing
        // handles remaining imbalance without scanning every worker here.
        auto peer = (index + workers.size() / 2) % workers.size();
        if (peer != index && workers[peer]->load() < workers[index]->load())
            index = peer;
    }
    auto completion = std::make_shared<Completion>(TaskId{++next_task});
    auto *selected = workers[index];
    selected->push(PendingTask{std::move(task), completion}, pinned);
    if (!pinned && !selected->idle())
        endpoint->wake_idle();
    return Result<TaskHandle>(TaskHandle(std::move(completion)));
}
} // namespace detail
} // namespace zco

namespace zco {
struct Runtime::Impl {
    RuntimeOptions options;
    std::shared_ptr<detail::Submission> submission =
        std::make_shared<detail::Submission>();
    std::vector<std::unique_ptr<detail::Worker>> workers;
    std::mutex join_mutex;

    explicit Impl(RuntimeOptions config) : options(config) {
        if (!options.worker_count)
            options.worker_count =
                std::max(1u, std::thread::hardware_concurrency());
        if (options.stack_size < 16 * 1024 ||
            options.stack_size > std::numeric_limits<size_t>::max() / 2 ||
            (options.stack_model != StackModel::kShared &&
             options.stack_model != StackModel::kIndependent) ||
            (options.stack_model == StackModel::kShared &&
             !options.shared_stack_count))
            throw std::invalid_argument("Invalid runtime stack configuration");
        for (size_t i = 0; i < options.worker_count; ++i)
            workers.emplace_back(
                new detail::Worker(i, options, submission,
                                   std::make_shared<detail::EpollReactor>()));
        for (auto &worker : workers)
            submission->workers.push_back(worker.get());
        for (auto &worker : workers)
            worker->set_peers(submission->workers);
        auto startup = std::make_shared<detail::Startup>();
        try {
            for (auto &worker : workers)
                worker->start(startup);
            std::unique_lock<std::mutex> lock(startup->mutex);
            startup->cv.wait(lock,
                             [&] { return startup->ready == workers.size(); });
            submission->accepting = true;
            startup->launch = true;
            startup->cv.notify_all();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(startup->mutex);
                startup->abort = true;
                startup->cv.notify_all();
            }
            for (auto &worker : workers)
                worker->join();
            throw;
        }
    }
};

Runtime::Runtime(RuntimeOptions options) : impl_(new Impl(options)) {}

Runtime::~Runtime() {
    request_stop();
    join();
}

Result<TaskHandle> Runtime::spawn(Task task) {
    return detail::submit(impl_->submission, std::move(task), 0, false);
}

Executor Runtime::executor(size_t index) const {
    if (index >= impl_->options.worker_count)
        throw std::out_of_range("Executor index");
    return Executor(impl_->submission, index);
}

size_t Runtime::worker_count() const { return impl_->options.worker_count; }

const RuntimeOptions &Runtime::options() const { return impl_->options; }

void Runtime::request_stop() {
    std::lock_guard<std::mutex> lock(impl_->submission->mutex);
    impl_->submission->accepting = false;
    for (auto &worker : impl_->workers)
        worker->stop();
}

void Runtime::join() {
    if (detail::current_worker &&
        detail::current_worker->submission().lock() == impl_->submission)
        throw std::logic_error("A runtime worker cannot join its own runtime");
    std::lock_guard<std::mutex> lock(impl_->join_mutex);
    for (auto &worker : impl_->workers)
        worker->join();
    std::lock_guard<std::mutex> endpoint_lock(impl_->submission->mutex);
    impl_->submission->workers.clear();
    impl_->workers.clear();
}

Result<TaskHandle> Executor::spawn(Task task) const {
    return detail::submit(endpoint_.lock(), std::move(task), index_, true);
}

bool Executor::valid() const {
    auto endpoint = endpoint_.lock();
    if (!endpoint)
        return false;
    std::lock_guard<std::mutex> lock(endpoint->mutex);
    return endpoint->accepting;
}

bool Executor::same_runtime(const Executor &other) const {
    return !endpoint_.owner_before(other.endpoint_) &&
           !other.endpoint_.owner_before(endpoint_);
}

TaskHandle::TaskHandle(std::shared_ptr<detail::Completion> state)
    : state_(std::move(state)) {}

TaskId TaskHandle::id() const { return state_ ? state_->id : TaskId{}; }

TaskStatus TaskHandle::status() const {
    if (!state_)
        throw std::logic_error("Empty task handle");
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}

std::exception_ptr TaskHandle::exception() const {
    if (!state_)
        throw std::logic_error("Empty task handle");
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->exception;
}

Result<void> TaskHandle::join(Deadline deadline) const {
    if (!state_)
        throw std::logic_error("Empty task handle");
    if (current_task_id() == state_->id)
        throw std::logic_error("A task cannot join itself");
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->status == TaskStatus::pending ||
        state_->status == TaskStatus::running) {
        if (!state_->waiters)
            state_->waiters.emplace();
        auto ticket = state_->waiters->add();
        lock.unlock();
        auto outcome = ticket.wait(deadline);
        lock.lock();
        state_->waiters->remove(ticket);
        if (outcome != WaitOutcome::ready)
            return Result<void>(wait_error(outcome));
    }
    if (state_->exception)
        std::rethrow_exception(state_->exception);
    if (state_->status == TaskStatus::canceled)
        return Result<void>(wait_error(WaitOutcome::canceled));
    return {};
}
} // namespace zco
