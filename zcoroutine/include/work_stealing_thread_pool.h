#ifndef ZCOROUTINE_WORK_STEALING_THREAD_POOL_H_
#define ZCOROUTINE_WORK_STEALING_THREAD_POOL_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zcoroutine_noncopyable.h"
#include "processor.h"
#include "stealable_queue_bitmap.h"

namespace zcoroutine {

class WorkStealingQueue;

/**
 * @brief 任务窃取线程池
 *
 * 负责：
 * - 管理 worker 线程生命周期
 * - 维护每个 worker 的 WorkStealingQueue 注册表（对外以 worker_id 索引）
 * - 提供 StealableQueueBitmap 以引导 enqueue/steal 的 victim 选择
 * - 提供启动屏障，确保 start() 返回前队列可用（可安全 enqueue）
 *
 * @note
 * 本类不关心 Task/Fiber 的语义；worker_entry 由上层（Scheduler）提供。
 * WorkStealingQueue 的创建/所有权由 Processor 持有，线程池只发布指针。
 */
class WorkStealingThreadPool : public NonCopyable {
public:
  explicit WorkStealingThreadPool(int thread_count = 1,
                                  std::string name = "WorkStealingThreadPool");
  ~WorkStealingThreadPool();

  int thread_count() const { return thread_count_; }
  const std::string &name() const { return name_; }

  StealableQueueBitmap &bitmap() { return stealable_bitmap_; }
  const StealableQueueBitmap &bitmap() const { return stealable_bitmap_; }

  uint32_t next_rr() {
    return rr_enqueue_.fetch_add(1, std::memory_order_relaxed);
  }

  /**
   * @brief 启动线程池。
   * @param worker_entry 由上层提供的 worker 入口函数，参数为 worker_id。
   * @note start() 在所有 worker 队列注册完成后返回。
   */
  void start(const std::function<void(int)> &worker_entry);

  /**
   * @brief 停止线程池。
   * @note 会先停止 work queues 并唤醒 waiters，然后 join 所有线程。
   */
  void stop();

  /**
   * @brief 获取指定 worker 的本地队列指针（由 Processor 持有）。
   */
  WorkStealingQueue *local_queue(int worker_id) const;

  /**
   * @brief 注册 worker 的本地队列指针。
   * @note 线程池不拥有队列，仅保存指针用于跨线程 steal / enqueue。
   */
  void register_work_queue(int worker_id, WorkStealingQueue *queue);

  /**
   * @brief 获取一个可用队列指针（可能为 nullptr，取决于启动/退出边界）。
   */
  WorkStealingQueue *get_next_queue(int worker_id) const;

  /**
   * @brief 停止所有队列并唤醒 waiters。
   */
  void stop_work_queues();

private:
  void wait_for_all_queues_registered();

private:
  std::string name_;
  int thread_count_{0};
  std::vector<std::unique_ptr<std::thread>> threads_;

  std::vector<std::unique_ptr<Processor>> processors_;

  std::atomic<uint32_t> rr_enqueue_{0};

  // 启动屏障：确保 start() 返回前，所有 worker 的 work queue 已注册。
  mutable std::mutex start_mutex_;
  std::condition_variable start_cv_;
  std::atomic<int> registered_worker_queues_{0};

  std::vector<std::atomic<WorkStealingQueue *>> work_queues_;

  StealableQueueBitmap stealable_bitmap_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_WORK_STEALING_THREAD_POOL_H_
