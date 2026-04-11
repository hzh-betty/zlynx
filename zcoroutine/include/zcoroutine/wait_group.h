#ifndef ZCOROUTINE_WAIT_GROUP_H_
#define ZCOROUTINE_WAIT_GROUP_H_

#include <atomic>
#include <cstdint>

#include "zcoroutine/event.h"

namespace zcoroutine {

/**
 * @brief 协程版计数等待器。
 * @details 用于等待一组并行任务全部完成，语义与 Go WaitGroup 接近。
 */
class WaitGroup {
  public:
    /**
     * @brief 构造等待组。
     * @param count 初始计数。
     */
    explicit WaitGroup(uint32_t count = 0);

    /**
     * @brief 增加计数。
     * @param count 增量，默认 1。
     * @return 无返回值。
     */
    void add(uint32_t count = 1);

    /**
     * @brief 完成一个任务。
     * @return 无返回值。
     */
    void done();

    /**
     * @brief 等待计数归零。
     * @return 无返回值。
     */
    void wait();

  private:
    std::atomic<int64_t> count_;
    Event done_event_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_WAIT_GROUP_H_
