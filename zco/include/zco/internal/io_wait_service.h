#ifndef ZCO_INTERNAL_IO_WAIT_SERVICE_H_
#define ZCO_INTERNAL_IO_WAIT_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace zco {
class Fiber;
class Poller;
class TimerQueue;
struct IoWaiter;

// 协调单次 I/O 等待的注册、超时、就绪和取消。
// 调度器提供挂起与恢复回调，I/O 操作通过 Poller 接口注入。
class IoWaitService {
  public:
    using Resume = std::function<void(const std::shared_ptr<Fiber> &, bool)>;
    using Park = std::function<bool()>;

    IoWaitService(int scheduler_id, TimerQueue &timers,
                  std::unique_ptr<Poller> poller, Resume resume);
    ~IoWaitService();

    bool start();
    void stop();
    void wake();
    void poll(int timeout_ms);
    bool wait(const std::shared_ptr<Fiber> &fiber, int fd, uint32_t events,
              uint32_t milliseconds, const Park &park);
    void cancel(int fd, int error);
    void handle_ready(const std::shared_ptr<IoWaiter> &waiter,
                      uint32_t ready_events);

  private:
    int scheduler_id_;
    TimerQueue &timers_;
    std::unique_ptr<Poller> poller_;
    Resume resume_;
};
} // 命名空间 zco

#endif
