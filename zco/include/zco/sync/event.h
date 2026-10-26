#ifndef ZLYNX_ZCO_INCLUDE_ZCO_SYNC_EVENT_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_SYNC_EVENT_H_

#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
/**
 * @brief 可由线程和协程等待的事件，对象须存活至所有操作结束。
 * @note 手动复位模式唤醒全部等待者并保持信号至 reset()；
 * 自动复位模式唤醒一个等待者，无等待者时为下一次 wait() 保留一次信号。
 */
class Event {
  public:
    explicit Event(bool manual_reset = false, bool signaled = false)
        : manual_reset_(manual_reset), signaled_(signaled) {}

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    /**
     * @brief 等待信号；超时或取消返回对应错误。
     */
    Result<void> wait(Deadline deadline = {});

    /**
     * @brief 设置信号。有有效等待者：完成其中一个等待。
        没有有效等待者：保存一个信号，让后续一次 wait() 直接通过。
     */
    void signal();

    void reset();

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    bool manual_reset_, signaled_;
};
} // namespace zco

#endif
