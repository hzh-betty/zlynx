#ifndef ZCOROUTINE_INTERNAL_EPOLLER_H_
#define ZCOROUTINE_INTERNAL_EPOLLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "zcoroutine/internal/poller.h"

namespace zcoroutine {

/**
 * @brief 单个 fd 的等待状态。
 * @details 同时维护读/写两个等待槽位，避免互相覆盖。
 */
struct FdWaitState {
    std::shared_ptr<IoWaiter> read_waiter;
    std::shared_ptr<IoWaiter> write_waiter;
    uint32_t registered_events;
    bool registered;

    FdWaitState()
        : read_waiter(), write_waiter(), registered_events(0),
          registered(false) {}
};

/**
 * @brief epoll 封装器。
 * @details 统一管理 epoll 生命周期、eventfd 唤醒和 waiter 列表。
 */
class Epoller : public Poller {
  public:
    /**
     * @brief 构造 epoller。
     * @param 无参数。
     * @return 无返回值。
     */
    Epoller();

    /**
     * @brief 析构 epoller。
     * @param 无参数。
     * @return 无返回值。
     */
    ~Epoller();

    /**
     * @brief 初始化 epoll 与 wake fd。
     * @param 无参数。
     * @return true 表示初始化成功。
     */
    bool start() override;

    /**
     * @brief 关闭 epoller 并释放资源。
     * @param 无参数。
     * @return 无返回值。
     */
    void stop() override;

    /**
     * @brief 唤醒正在 epoll_wait 的线程。
     * @param 无参数。
     * @return 无返回值。
     */
    void wake() override;

    /**
     * @brief 注册 waiter 到对应 fd 事件槽位。
     * @param waiter 等待请求对象。
     * @return true 表示注册成功。
     */
    bool register_waiter(const std::shared_ptr<IoWaiter> &waiter) override;

    /**
     * @brief 解除 waiter 注册。
     * @param waiter 等待请求对象。
     * @return 无返回值。
     */
    void unregister_waiter(const std::shared_ptr<IoWaiter> &waiter) override;

    /**
     * @brief 等待 IO 事件并回调处理。
     * @param timeout_ms 等待超时毫秒。
     * @param on_ready 事件就绪回调。
     * @return 无返回值。
     */
    void wait_events(
        int timeout_ms,
        const std::function<void(const std::shared_ptr<IoWaiter> &waiter,
                                 uint32_t ready_events)> &on_ready) override;

  private:
    bool update_interest_locked(int fd, FdWaitState *state);

    /**
     * @brief 清空 wake fd 中的唤醒计数。
     * @param 无参数。
     * @return 无返回值。
     */
    void consume_wakeup_fd();

    int epoll_fd_;
    int wake_fd_;
    std::atomic<bool> wake_pending_;

    std::mutex waiter_mutex_;
    std::unordered_map<int, FdWaitState> fd_wait_states_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_INTERNAL_EPOLLER_H_
