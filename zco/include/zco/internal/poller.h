#ifndef ZCO_INTERNAL_POLLER_H_
#define ZCO_INTERNAL_POLLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "zco/internal/fiber.h"
#include "zco/internal/noncopyable.h"
#include "zco/internal/timer.h"

namespace zco {

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

    virtual bool register_waiter(const std::shared_ptr<IoWaiter> &waiter) = 0;
    virtual void unregister_waiter(const std::shared_ptr<IoWaiter> &waiter) = 0;

    /**
     * @brief 等待 I/O 事件
     * @param timeout_ms 超时时间（毫秒）
     * @param on_ready 事件就绪回调函数
     */
    virtual void wait_events(
        int timeout_ms,
        const std::function<void(const std::shared_ptr<IoWaiter> &waiter,
                                 uint32_t ready_events)> &on_ready) = 0;
};

std::unique_ptr<Poller> create_default_poller();

} // namespace zco

#endif // ZCO_INTERNAL_POLLER_H_