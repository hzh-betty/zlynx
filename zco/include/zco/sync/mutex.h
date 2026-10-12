#ifndef ZLYNX_ZCO_INCLUDE_ZCO_SYNC_MUTEX_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_SYNC_MUTEX_H_

#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
/**
 * @brief 在线程中阻塞、在协程中挂起的非递归互斥锁。
 * @note 对象须存活至所有加锁、解锁操作结束；只有获得锁的一方可调用 unlock()。
 */
class Mutex {
  public:
    Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    /**
     * @brief 获取锁；等待超时或取消时不获得所有权，并返回对应错误。
     */
    Result<void> lock(Deadline deadline = {});
    bool try_lock();

    /**
     * @brief 释放或向下一个等待者转交锁。
     * @pre 调用方已通过 lock() 或 try_lock() 获得锁。
     * @throws std::logic_error 锁处于未加锁状态。
     */
    void unlock();

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    bool locked_ = false;
};
} // namespace zco

#endif
