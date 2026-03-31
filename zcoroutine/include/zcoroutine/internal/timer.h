#ifndef ZCOROUTINE_INTERNAL_TIMER_H_
#define ZCOROUTINE_INTERNAL_TIMER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "zcoroutine/internal/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 定时器取消令牌。
 * @details 当 cancelled 为 true 时，定时器回调将被跳过。
 */
struct TimerToken {
  std::atomic<bool> cancelled{false};
};

/**
 * @brief 定时器优先队列。
 * @details 按 deadline 与 sequence 顺序触发到期回调。
 */
class TimerQueue : public NonCopyable {
 public:
  /**
   * @brief 构造定时器队列。
   * @param 无参数。
   * @return 无返回值。
   */
  TimerQueue();

  /**
   * @brief 添加定时器回调。
   * @param milliseconds 延时毫秒。
   * @param callback 到期回调函数。
   * @return 定时器取消令牌。
   */
  std::shared_ptr<TimerToken> add_timer(uint32_t milliseconds, std::function<void()> callback);

  /**
   * @brief 执行所有已到期定时器。
   * @param 无参数。
   * @return 无返回值。
   */
  void process_due();

  /**
   * @brief 获取下一次等待超时时间。
   * @param 无参数。
   * @return 下一次等待毫秒值。
   */
  int next_timeout_ms() const;

 private:
  /**
   * @brief 定时器条目。
   * @details 包含触发时间、序号、取消令牌与回调。
   */
  struct TimerEntry {
    uint64_t deadline_ms;
    uint64_t sequence;
    std::shared_ptr<TimerToken> token;
    std::function<void()> callback;
  };

  /**
   * @brief 定时器比较器。
   * @details 使优先队列按最近 deadline 优先出队。
   */
  struct TimerCompare {
    /**
     * @brief 比较两个定时器条目优先级。
     * @param lhs 左侧条目。
     * @param rhs 右侧条目。
     * @return true 表示 lhs 优先级低于 rhs。
     */
    bool operator()(const TimerEntry& lhs, const TimerEntry& rhs) const {
      if (lhs.deadline_ms != rhs.deadline_ms) {
        return lhs.deadline_ms > rhs.deadline_ms;
      }
      return lhs.sequence > rhs.sequence;
    }
  };

  mutable std::mutex mutex_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare> timers_;
  std::atomic<uint64_t> sequence_;
};

/**
 * @brief 获取当前单调时钟毫秒值。
 * @param 无参数。
 * @return 当前毫秒时间戳。
 */
uint64_t now_ms();

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_TIMER_H_
