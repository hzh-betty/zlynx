#include "wait/wait_state.h"

namespace zco {
namespace detail {
bool WaitState::complete(WaitOutcome outcome) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_)
            return false;
        outcome_ = outcome;
        completed_ = true;
    }
    cv_.notify_all();
    if (auto endpoint = endpoint_.lock())
        endpoint->notify(id_);
    return true;
}

bool WaitState::completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
}

WaitOutcome WaitState::outcome() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_)
        throw std::logic_error("Wait has not completed");
    return outcome_;
}

WaitOutcome WaitState::wait_thread(Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!completed_) {
        if (deadline.is_infinite())
            cv_.wait(lock);
        else if (cv_.wait_until(lock, deadline.time()) ==
                 std::cv_status::timeout) {
            lock.unlock();
            complete(WaitOutcome::timeout);
            lock.lock();
        }
    }
    return outcome_;
}
} // namespace detail
} // namespace zco

namespace zco {
bool WakeToken::complete(WaitOutcome outcome) const {
    auto state = state_.lock();
    return state && state->complete(outcome);
}
} // namespace zco
