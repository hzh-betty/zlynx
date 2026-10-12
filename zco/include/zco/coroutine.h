#ifndef ZLYNX_ZCO_INCLUDE_ZCO_COROUTINE_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_COROUTINE_H_

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

/**
 * @brief 弱引用一次等待状态的完成令牌，不延长等待状态生命周期。
 */
class WakeToken {
  public:
    WakeToken() = default;

    explicit WakeToken(std::weak_ptr<detail::WaitState> state)
        : state_(std::move(state)) {}

    /**
     * @brief 尝试完成该次等待。
     * @return 状态已过期或已完成时返回 false，首次完成时返回 true。
     */
    bool complete(WaitOutcome outcome = WaitOutcome::ready) const;

  private:
    std::weak_ptr<detail::WaitState> state_;
};

bool in_coroutine();

/**
 * @brief 返回当前协程任务身份；普通线程返回空身份。
 */
TaskId current_task_id();
Executor current_executor();

/**
 * @brief 协程让出执行给调度器；普通线程调用 std::this_thread::yield()。
 */
void yield();

/**
 * @brief 等待截止时间，协程挂起，普通线程阻塞。
 * @return 到期视为成功；运行时取消等待时返回对应错误。
 * @note 无限截止时间不会自行到期。
 */
Result<void> sleep_until(Deadline deadline);

template <class Rep, class Period>
Result<void> sleep_for(std::chrono::duration<Rep, Period> duration) {
    return sleep_until(Deadline::after(duration));
}
} // namespace zco

#endif
