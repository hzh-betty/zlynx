#ifndef ZCOROUTINE_IO_EVENT_H_
#define ZCOROUTINE_IO_EVENT_H_

#include <cstdint>

#include "zcoroutine/sched.h"

namespace zcoroutine {

/**
 * @brief IO 事件类型。
 */
enum class IoEventType : uint32_t {
  kRead = 0x001,
  kWrite = 0x004,
};

/**
 * @brief 协程 IO 事件等待器。
 * @details 在 Linux 下基于 epoll 实现。
 */
class IoEvent {
 public:
  /**
   * @brief 构造 IO 事件等待器。
   * @param fd 文件描述符。
   * @param event_type 等待事件类型。
   */
  IoEvent(int fd, IoEventType event_type);

  /**
   * @brief 析构函数。
   */
  ~IoEvent();

  /**
   * @brief 等待事件到达或超时。
   * @param milliseconds 超时毫秒。
   * @return true 表示事件到达，false 表示超时或错误。
   */
  bool wait(uint32_t milliseconds = kInfiniteTimeoutMs);

 private:
  int fd_;
  IoEventType event_type_;
  bool added_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_IO_EVENT_H_
