#ifndef ZCOROUTINE_WORK_STEALING_QUEUE_H_
#define ZCOROUTINE_WORK_STEALING_QUEUE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "spinlock.h"
#include "stealable_queue_bitmap.h"
#include "task_queue.h"
#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

/**
 * @brief 每线程本地队列 + 窃取队列
 *
 * - owner 线程：push_back / pop_back（LIFO，提升缓存局部性）
 * - thief 线程：从 front 批量 steal（FIFO，减少与 owner 争用）
 *
 */
class WorkStealingQueue : public NonCopyable {
public:
  WorkStealingQueue() = default;
  ~WorkStealingQueue() = default;

  /**
   * @brief 绑定可窃取队列提示位图，用于在队列规模跨越阈值时上报“可窃取”状态。
    * @note 通常仅 worker 本地队列需要绑定。
   */
  void bind_bitmap(StealableQueueBitmap *bitmap, int worker_id,
                   size_t high_watermark, size_t low_watermark);

  void push(Task &&task);

  /**
   * @brief owner 批量 pop（LIFO）
   * @param out 输出任务数组指针
   * @param max_count 最大取出任务数量
   * @return 实际取出的任务数量
   */
  size_t pop_batch(Task *out, size_t max_count);

  /**
   * @brief thief 批量 steal（FIFO）
   * @param out 输出任务数组指针
   * @param max_count 最大取出任务数量
   * @return 实际取出的任务数量
   */
  size_t steal_batch(Task *out, size_t max_count);

  /**
   * @brief 若本队列为空，则与 victim 交换底层 deque（O(1)）。
   * @return 交换后本队列获得的任务数；若未发生交换返回 0。
   */
  size_t swap_if_empty(WorkStealingQueue &victim);

  /**
   * @brief 等待直到队列非空或停止，然后 owner 批量 pop。
   * @param out 输出任务数组指针
   * @param max_count 最大取出任务数量
   * @param timeout_ms 超时时间，单位毫秒
   * @return 实际取出的任务数量，0 表示超时或已停止且无任务。
   */
  size_t wait_pop_batch(Task *out, size_t max_count, int timeout_ms);

  /**
   * @brief 停止队列，唤醒所有等待的线程
   */
  void stop();

  /**
   * @brief 唤醒一个在 wait_pop_batch 上等待的线程（若有等待者）
   */
  void notify_one();

  /**
   * @brief 近似获取队列大小（非严格准确）
   * @return 队列中任务数量
   */
  size_t approx_size() const { return size_.load(std::memory_order_relaxed); }

  /**
   * @brief 判断队列是否为空
   * @return true表示空，false表示非空
   */
  bool empty() const { return approx_size() == 0; }

private:
  void maybe_update_bitmap(size_t new_size);

  mutable Spinlock lock_;
  std::mutex wait_mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;

  std::atomic<size_t> size_{0};
  std::atomic<int> waiters_{0};
  std::atomic<bool> stopped_{false};

  StealableQueueBitmap *bitmap_{nullptr};
  int bitmap_worker_id_{-1};
  size_t high_watermark_{0};
  size_t low_watermark_{0};
  std::atomic<bool> bitmap_published_{false};
};

} // namespace zcoroutine

#endif // ZCOROUTINE_WORK_STEALING_QUEUE_H_
