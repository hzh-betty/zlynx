#ifndef ZCOROUTINE_INTERNAL_PROCESSOR_H_
#define ZCOROUTINE_INTERNAL_PROCESSOR_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "zcoroutine/internal/context.h"
#include "zcoroutine/internal/fiber_pool.h"
#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/noncopyable.h"
#include "zcoroutine/internal/poller.h"
#include "zcoroutine/internal/shared_stack_buffer.h"
#include "zcoroutine/internal/snapshot_buffer_pool.h"
#include "zcoroutine/internal/steal_queue.h"
#include "zcoroutine/internal/timer.h"
#include "zcoroutine/sched.h"

namespace zcoroutine {


  static constexpr size_t kSharedStackGroupSize = 8; // 每个处理器的共享栈数量，实际使用时可根据需求调整
  static constexpr size_t kSnapshotBucketCount = 8; // 快照缓冲池桶数量，分桶管理不同大小的快照，减少内存碎片
  static constexpr uint8_t kDynamicSnapshotBucket = 0xff; // 动态快照桶标识，表示不固定大小的快照需要单独分配
  static constexpr size_t kSnapshotPoolPerBucketLimit = 256; // 每个桶的快照缓冲池最大容量，超过后不再缓存，避免过度占用内存

/**
 * @brief 调度处理器。
 * @details 每个处理器绑定一个线程，负责 Fiber 调度、定时器和 IO 事件。
 */
class Processor : public NonCopyable {
 public:
  /**
  * @brief 构造处理器。
  * @param id 处理器编号。
  * @param stack_size 共享栈大小。
  * @return 无返回值。
  */
  Processor(int id, size_t stack_size);
  Processor(int id, size_t stack_size, size_t shared_stack_num, StackModel stack_model);

  /**
  * @brief 析构处理器。
  * @param 无参数。
  * @return 无返回值。
  */
  ~Processor();

  /**
  * @brief 启动处理器线程。
  * @param 无参数。
  * @return 无返回值。
  */
  void start();

  /**
  * @brief 请求停止处理器线程。
  * @param 无参数。
  * @return 无返回值。
  */
  void stop();

  /**
  * @brief 等待处理器线程退出。
  * @param 无参数。
  * @return 无返回值。
  */
  void join();

  /**
  * @brief 提交待创建任务。
  * @param task 任务函数。
  * @return 无返回值。
  */
  void enqueue_task(Task task);

  /**
  * @brief 将 Fiber 放入就绪队列。
  * @param fiber 协程对象。
  * @return 无返回值。
  */
  void enqueue_ready(Fiber::ptr fiber);

  /**
  * @brief 窃取一批待创建任务。
  * @param tasks 输出任务队列。
  * @param max_steal 最大窃取数。
  * @param min_reserve 给受害者保留的最小任务数。
  * @return 实际窃取数量。
  */
  size_t steal_tasks(std::deque<Task>* tasks, size_t max_steal, size_t min_reserve);

  /**
  * @brief 获取待创建任务数量。
  * @param 无参数。
  * @return 任务数。
  */
  uint32_t pending_task_count() const;

  /**
  * @brief 获取处理器编号。
  * @param 无参数。
  * @return 处理器编号。
  */
  int id() const;

  /**
  * @brief 获取当前运行 Fiber。
  * @param 无参数。
  * @return 当前 Fiber 指针。
  */
  Fiber::ptr current_fiber() const;

  /**
  * @brief 获取调度器上下文。
  * @param 无参数。
  * @return 上下文指针。
  */
  ucontext_t* scheduler_context();

  /**
  * @brief 当前 Fiber 主动让出执行权。
  * @param 无参数。
  * @return 无返回值。
  */
  void yield_current();

  /**
  * @brief 将当前 Fiber 标记为等待态。
  * @param 无参数。
  * @return 无返回值。
  */
  void prepare_wait_current();

  /**
  * @brief 挂起当前 Fiber。
  * @param 无参数。
  * @return true 表示非超时恢复。
  */
  bool park_current();

  /**
  * @brief 挂起当前 Fiber 并设置超时。
  * @param milliseconds 超时毫秒。
  * @return true 表示非超时恢复。
  */
  bool park_current_for(uint32_t milliseconds);

  /**
  * @brief 添加定时器。
  * @param milliseconds 延时毫秒。
  * @param callback 到期回调。
  * @return 定时器令牌。
  */
  std::shared_ptr<TimerToken> add_timer(uint32_t milliseconds, std::function<void()> callback);

  /**
  * @brief 等待 fd 事件。
  * @param fd 文件描述符。
  * @param events 事件掩码。
  * @param milliseconds 超时毫秒。
  * @return true 表示事件已到达。
  */
  bool wait_fd(int fd, uint32_t events, uint32_t milliseconds);

  /**
  * @brief 获取共享栈起始地址。
  * @param stack_slot 共享栈槽位。
  * @param 无参数。
  * @return 栈地址。
  */
  void* shared_stack_data(size_t stack_slot = 0);

  /**
  * @brief 获取共享栈大小。
  * @param stack_slot 共享栈槽位。
  * @param 无参数。
  * @return 栈大小（字节）。
  */
  size_t shared_stack_size(size_t stack_slot = 0) const;

  /**
  * @brief 获取共享栈槽位数量。
  * @param 无参数。
  * @return 槽位数量。
  */
  size_t shared_stack_count() const;

  /**
   * @brief 获取当前处理器栈模型。
   * @param 无参数。
   * @return 栈模型。
   */
  StackModel stack_model() const;

  /**
  * @brief 获取就绪与待创建任务总量。
  * @param 无参数。
  * @return 任务负载。
  */
  uint32_t queue_load() const;

  /**
  * @brief 获取累计运行时间（纳秒）。
  * @param 无参数。
  * @return 运行时间。
  */
  uint64_t cpu_time_ns() const;

  /**
  * @brief 获取混合负载分数。
  * @param 无参数。
  * @return 分数，越小越空闲。
  */
  uint64_t load_score() const;

  /**
  * @brief 申请分档快照缓冲。
  * @param required_size 需要容量。
  * @param capacity 输出实际容量。
  * @param bucket_index 输出桶编号。
  * @return 缓冲地址。
  */
  char* acquire_snapshot_buffer(size_t required_size, size_t* capacity, uint8_t* bucket_index);

  /**
  * @brief 归还分档快照缓冲。
  * @param buffer 缓冲地址。
  * @param bucket_index 桶编号。
  * @param capacity 缓冲容量。
  * @return 无返回值。
  */
  void release_snapshot_buffer(char* buffer, uint8_t bucket_index, size_t capacity);

 private:
  /**
   * @brief 处理器主循环。
   * @param 无参数。
   * @return 无返回值。
   */
  void run_loop();

  /**
   * @brief 唤醒调度循环。
   * @param 无参数。
   * @return 无返回值。
   */
  void wake_loop();

  /**
   * @brief 拉取待创建任务并实例化 Fiber。
   * @param 无参数。
   * @return 无返回值。
   */
  void drain_new_tasks();

  /**
   * @brief 执行就绪队列中的 Fiber。
   * @param 无参数。
   * @return 无返回值。
   */
  void run_ready_tasks();

  bool dequeue_ready_fiber(Fiber::ptr* fiber);

  bool recycle_if_done_before_run(const Fiber::ptr& fiber);

  Fiber::ptr switch_to_fiber(Fiber::ptr fiber);

  Fiber::State finalize_after_switch(const Fiber::ptr& fiber);

  void dispatch_resumed_fiber(Fiber::ptr fiber, Fiber::State state);

  bool has_ready_tasks() const;

  void wait_io_events_when_idle();

  void steal_tasks_when_idle();

  void update_load_metrics(uint64_t loop_ns);

  /**
   * @brief 处理到期定时器。
   * @param 无参数。
   * @return 无返回值。
   */
  void process_timers();

  /**
   * @brief 计算 epoll 等待超时。
   * @param 无参数。
   * @return 超时毫秒值。
   */
  int next_timeout_ms() const;

  /**
   * @brief 处理单个 IO 就绪事件。
   * @param waiter 等待请求对象。
   * @return 无返回值。
   */
  void handle_io_ready(const std::shared_ptr<IoWaiter>& waiter, uint32_t ready_events);

  /**
   * @brief 保存 Fiber 共享栈快照。
   * @param fiber 协程对象。
   * @return 无返回值。
   */
  void save_fiber_stack(const Fiber::ptr& fiber);

  /**
   * @brief 恢复 Fiber 共享栈快照。
   * @param fiber 协程对象。
   * @return 无返回值。
   */
  void restore_fiber_stack(const Fiber::ptr& fiber);

  Fiber::ptr obtain_fiber(Task task);

  void recycle_fiber(const Fiber::ptr& fiber);

  void enqueue_stolen_tasks(std::deque<Task>* tasks);

  void enqueue_ready_batch(std::deque<Fiber::ptr>* fibers);

  int id_;
  const size_t stack_size_;
  const StackModel stack_model_;
  std::atomic<bool> running_;
  std::thread worker_;

  std::atomic<uint32_t> ready_size_; // 就绪队列长度，近似值仅供负载评估，不强求精确
  std::atomic<uint64_t> cpu_time_ns_;// 累计运行时间，纳秒级，供负载评估使用
  std::atomic<uint64_t> ema_loop_ns_; // 调度循环平均耗时，纳秒级，供负载评估使用

  mutable std::mutex run_queue_mutex_;
  std::deque<Fiber::ptr> run_queue_;

  StealQueue steal_queue_;

  FiberPool fiber_pool_;
  std::atomic<size_t> next_stack_slot_;

  SnapshotBufferPool snapshot_pool_;

  size_t steal_probe_cursor_; // 窃取探测游标，轮询选择窃取对象，避免总是从同一处理器窃取导致负载不均

  TimerQueue timer_queue_;
  std::unique_ptr<Poller> poller_;

  SharedStackPool shared_stacks_;

  Context scheduler_context_;
  Fiber::ptr current_fiber_;
};

/**
 * @brief 获取当前线程绑定的处理器。
 * @param 无参数。
 * @return 处理器指针，未绑定则返回 nullptr。
 */
Processor* current_processor();

/**
 * @brief 绑定当前线程处理器。
 * @param processor 处理器指针。
 * @return 无返回值。
 */
void set_current_processor(Processor* processor);

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_PROCESSOR_H_
