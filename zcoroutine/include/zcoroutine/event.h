#ifndef ZCOROUTINE_EVENT_H_
#define ZCOROUTINE_EVENT_H_

#include <cstdint>
#include <memory>

#include "zcoroutine/sched.h"

namespace zcoroutine {

/**
 * @brief 协程与线程可共享的事件同步原语。
 * @details 支持 auto reset 与 manual reset 两种模式，可用于 wait/signal/reset。
 */
class Event {
 public:
  /**
   * @brief 构造事件对象。
   * @param manual_reset true 表示手动复位，false 表示自动复位。
   * @param signaled 初始是否为已触发状态。
   */
  explicit Event(bool manual_reset = false, bool signaled = false);

  /**
   * @brief 析构事件对象。
   */
  ~Event();

  /**
   * @brief 拷贝构造，内部共享同一事件状态。
   * @param other 另一个事件对象。
   */
  Event(const Event& other);

  /**
   * @brief 移动构造。
   * @param other 被移动对象。
   */
  Event(Event&& other) noexcept;

  /**
   * @brief 等待事件触发。
   * @param milliseconds 超时毫秒，默认无限等待。
   * @return true 表示成功触发，false 表示超时。
   */
  bool wait(uint32_t milliseconds = kInfiniteTimeoutMs) const;

  /**
   * @brief 触发事件。
   * @return 无返回值。
   */
  void signal() const;

  /**
   * @brief 重置为未触发状态。
   * @return 无返回值。
   */
  void reset() const;

  /**
   * @brief 唤醒全部等待者，仅用于内部组合场景。
   * @return 无返回值。
   */
  void notify_all() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_EVENT_H_
