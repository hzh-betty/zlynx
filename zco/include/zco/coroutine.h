#pragma once
#include "zco/runtime.h"

namespace zco {
namespace detail {
class WaitState;
}

struct WaitId {
    TaskId task;
    uint64_t generation = 0;
};

inline bool operator==(WaitId a, WaitId b) {
    return a.task == b.task && a.generation == b.generation;
}

class WakeToken {
  public:
    WakeToken() = default;

    explicit WakeToken(std::weak_ptr<detail::WaitState> state)
        : state_(std::move(state)) {}

    bool complete(WaitOutcome outcome = WaitOutcome::ready) const;

  private:
    std::weak_ptr<detail::WaitState> state_;
};

bool in_coroutine();
TaskId current_task_id();
Executor current_executor();
void yield();
Result<void> sleep_until(Deadline deadline);

template <class Rep, class Period>
Result<void> sleep_for(std::chrono::duration<Rep, Period> duration) {
    return sleep_until(Deadline::after(duration));
}
} // namespace zco
