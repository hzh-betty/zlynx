#ifndef ZCOROUTINE_INTERNAL_POLLER_H_
#define ZCOROUTINE_INTERNAL_POLLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/noncopyable.h"
#include "zcoroutine/internal/timer.h"

namespace zcoroutine {

/**
 * @brief 单个 fd 等待请求。
 * @details 描述当前等待的事件类型、目标协程与超时定时器。
 */
struct IoWaiter {
  int fd;
  uint32_t events;
  std::weak_ptr<Fiber> fiber;
  std::shared_ptr<TimerToken> timer;
  std::atomic<bool> active;
};

/**
 * @brief IO 多路复用抽象。
 * @details 统一管理等待器注册、唤醒与事件分发。
 */
class Poller : public NonCopyable {
 public:
  Poller() = default;
  virtual ~Poller() = default;

  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual void wake() = 0;

  virtual bool register_waiter(const std::shared_ptr<IoWaiter>& waiter) = 0;
  virtual void unregister_waiter(const std::shared_ptr<IoWaiter>& waiter) = 0;

  virtual void wait_events(
      int timeout_ms,
      const std::function<void(const std::shared_ptr<IoWaiter>& waiter, uint32_t ready_events)>&
          on_ready) = 0;
};

std::unique_ptr<Poller> create_default_poller();

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_POLLER_H_