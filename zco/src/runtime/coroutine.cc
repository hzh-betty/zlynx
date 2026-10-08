#include "zco/coroutine.h"
#include "runtime/worker.h"
#include <atomic>

namespace zco {
namespace detail {
std::shared_ptr<WaitState> prepare_wait() {
    if (current_worker && current_worker->current_id())
        return current_worker->make_wait();
    static std::atomic<uint64_t> thread_wait{0};
    return std::make_shared<WaitState>(WaitId{TaskId{}, ++thread_wait});
}

WaitOutcome park_wait(const std::shared_ptr<WaitState> &state,
                      Deadline deadline) {
    if (current_worker && current_worker->current_id())
        return current_worker->park(state, deadline);
    return state->wait_thread(deadline);
}

Result<void>
current_wait_ready(const std::shared_ptr<io::detail::Resource> &resource,
                   io::Interest interest, Deadline deadline) {
    if (!current_worker || !current_worker->current_id())
        return Result<void>(
            std::make_error_code(std::errc::operation_not_permitted));
    return current_worker->wait_io(resource, interest, deadline);
}
} // namespace detail
} // namespace zco

namespace zco {
bool in_coroutine() { return bool(current_task_id()); }

TaskId current_task_id() {
    return detail::current_worker ? detail::current_worker->current_id()
                                  : TaskId{};
}

Executor current_executor() {
    auto *worker = detail::current_worker;
    return worker ? Executor(worker->submission(), worker->index())
                  : Executor{};
}

void yield() {
    if (in_coroutine())
        detail::current_worker->yield_current();
    else
        std::this_thread::yield();
}

Result<void> sleep_until(Deadline deadline) {
    auto wait = detail::prepare_wait();
    auto outcome = detail::park_wait(wait, deadline);
    return outcome == WaitOutcome::timeout ? Result<void>{}
                                           : Result<void>(wait_error(outcome));
}
} // namespace zco
