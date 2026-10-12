#ifndef ZLYNX_ZCO_INCLUDE_ZCO_SYNC_WAIT_GROUP_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_SYNC_WAIT_GROUP_H_

#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
/**
 * @brief 维护未完成工作计数，计数降至零时唤醒等待者。
 * @note 对象须存活至所有操作结束；工作开始前先登记计数，完成后调用 done()。
 */
class WaitGroup {
  public:
    explicit WaitGroup(size_t count = 0) : count_(count) {}

    WaitGroup(const WaitGroup &) = delete;
    WaitGroup &operator=(const WaitGroup &) = delete;

    /**
     * @brief 增加未完成工作计数。
     * @throws std::overflow_error 增加后计数无法由 size_t 表示。
     */
    void add(size_t count = 1);

    /**
     * @brief 完成一项已登记工作。
     * @throws std::logic_error 当前计数为零。
     */
    void done();

    /**
     * @brief 等待计数降至零；超时或取消返回对应错误，不改变计数。
     */
    Result<void> wait(Deadline deadline = {});

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    size_t count_;
};
} // namespace zco

#endif
