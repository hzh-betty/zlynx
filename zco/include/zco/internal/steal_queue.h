#ifndef ZCO_INTERNAL_STEAL_QUEUE_H_
#define ZCO_INTERNAL_STEAL_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>

#include "zco/internal/noncopyable.h"
#include "zco/sched.h"

namespace zco {

/**
 * @brief 任务窃取队列
 * @details
 * - StealQueue 是一个线程安全的双端队列，支持一个生产者线程和多个消费者线程。
 * - 生产者线程通过 push() 向队列尾部添加任务。
 */
class StealQueue : public NonCopyable {
  public:
    StealQueue();
    void push(Task task);

    /**
     * @brief 窃取任务
     * @param tasks 任务队列
     * @param max_steal 最大窃取数量
     * @param min_reserve 最小保留数量
     * @return 窃取的任务数量
     */
    size_t steal(std::deque<Task> *tasks, size_t max_steal, size_t min_reserve);

    /**
     * @brief 清空所有任务
     * @param tasks 任务队列
     */
    void drain_all(std::deque<Task> *tasks);

    /**
     * @brief 清空部分任务
     * @param tasks 任务队列
     * @param max_count 最大清空数量
     */
    void drain_some(std::deque<Task> *tasks, size_t max_count);

    /**
     * @brief 将队列中的任务追加到另一个队列
     * @param tasks 目标任务队列
     */
    void append(std::deque<Task> *tasks);

    size_t size() const;

  private:
    mutable std::mutex mutex_;
    std::atomic<size_t> size_;
    std::deque<Task> tasks_;
};

} // namespace zco

#endif // ZCO_INTERNAL_STEAL_QUEUE_H_
