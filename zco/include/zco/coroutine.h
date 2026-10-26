#ifndef ZLYNX_ZCO_INCLUDE_ZCO_COROUTINE_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_COROUTINE_H_

#include "zco/runtime.h"

namespace zco {
namespace detail {
class WaitState;
}

/**
 * @brief WaitId 保证通知必须属于任务当前这一轮等待，避免过期通知误唤醒。
 * 因为当前代码存在一条路径：Worker 可以直接观察到等待已完成，提前把任务入队，而邮箱（Inbox）里的通知还没被消费。
 * （即如果通知发生在检查之后、切换期间，再由调度器检查, 防止丢失唤醒。）
 */
struct WaitId {
    TaskId task;
    uint64_t generation = 0;
};

inline bool operator==(WaitId a, WaitId b) {
    return a.task == b.task && a.generation == b.generation;
}

/**
 * @brief WakeToken 可以理解成“某一次等待的唤醒句柄”。
 * 它存在的目的，是让不同的事件源用同一种方式结束等待：
 * 协程创建 WaitState
        ↓
   把 WakeToken 交给定时器 / I/O / WaitQueue
        ↓
   某个事件发生，调用 token.complete(...)
        ↓
   WaitState 只接受第一个结果
        ↓
   通知 Worker，协程恢复执行
 */
class WakeToken {
  public:
    WakeToken() = default;

    explicit WakeToken(std::weak_ptr<detail::WaitState> state)
        : state_(std::move(state)) {}

    /**
     * @brief 尝试完成该次等待。I/O、超时、取消可能同时发生
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
