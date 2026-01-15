#ifndef ZCOROUTINE_SCHEDULER_H_
#define ZCOROUTINE_SCHEDULER_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "runtime/fiber.h"
#include "scheduling/stealable_queue_bitmap.h"
#include "scheduling/task_queue.h" // for Task
#include "util/thread_context.h"

namespace zcoroutine {

class WorkStealingQueue;

/**
 * @brief 调度器类
 * 基于线程池的M:N调度模型
 * 使用std::thread和std::mutex，不再封装Thread/Mutex类
 */
class Scheduler {
public:
  using ptr = std::shared_ptr<Scheduler>;

  /**
   * @brief 构造函数
   * @param thread_count 线程数量
   * @param name 调度器名称
   * @param use_shared_stack 是否使用共享栈模式
   */
  explicit Scheduler(int thread_count = 1, std::string name = "Scheduler",
                     bool use_shared_stack = false);

  /**
   * @brief 析构函数
   */
  virtual ~Scheduler();

  /**
   * @brief 获取调度器名称
   */
  const std::string &name() const { return name_; }

  /**
   * @brief 启动调度器
   */
  void start();

  /**
   * @brief 停止调度器
   * 等待所有任务执行完毕后停止
   */
  void stop();

  /**
   * @brief 调度协程（拷贝版本）
   * @param fiber 协程指针
   */
  void schedule(const Fiber::ptr &fiber);

  /**
   * @brief 调度协程（移动版本，性能优化）
   * @param fiber 协程指针（右值引用）
   */
  void schedule(Fiber::ptr &&fiber);

  /**
   * @brief 模板方法：调度可调用对象
   * @tparam F 函数类型
   * @tparam Args 参数类型
   * @note 使用 SFINAE 排除 Fiber::ptr 类型，避免与 schedule(Fiber::ptr) 冲突
   */
  template <class F, class... Args,
            typename = typename std::enable_if<!std::is_same<
                typename std::decay<F>::type, Fiber::ptr>::value>::type>
  void schedule(F &&f, Args &&...args) {
    auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    enqueue(Task(std::move(func)));
  }

  /**
   * @brief 是否正在运行
   */
  bool is_running() const { return !stopping_.load(std::memory_order_relaxed); }

  /**
   * @brief 获取待处理任务数
   */
  size_t pending_task_count() const {
    return pending_tasks_.load(std::memory_order_relaxed);
  }

  /**
   * @brief 获取当前调度器（线程本地）
   */
  static Scheduler *get_this();

  /**
   * @brief 设置当前调度器（线程本地）
   */
  static void set_this(Scheduler *scheduler);

  /**
   * @brief 检查是否使用共享栈模式
   * @return true表示使用共享栈，false表示使用独立栈
   */
  bool is_shared_stack() const { return use_shared_stack_; }

  /**
   * @brief 获取共享栈指针（仅当共享栈模式时有效）
   * @return 共享栈指针
   */
  SharedStack *get_shared_stack() const {
    return ThreadContext::get_shared_stack();
  }

private:
  /**
   * @brief 内部方法：将任务加入调度队列
   * @param task 任务对象
   */
  void enqueue(Task &&task);

  /**
   * @brief 内部方法：获取指定worker的工作队列指针
   * @param worker_id worker线程ID
   * @return 工作队列指针，若无则返回nullptr
   */
  void register_work_queue(int worker_id, WorkStealingQueue *queue);

  /**
   * @brief 内部方法：获取指定worker的工作队列指针
   * @param worker_id worker线程ID
   * @return 工作队列指针，若无则返回nullptr
   */
  WorkStealingQueue *get_next_queue(int worker_id) const;

  /**
   * @brief 内部方法：停止所有工作队列
   */
  void stop_work_queues();

protected:
  /**
   * @brief 工作线程主循环
   * 初始化main_fiber和scheduler_fiber，然后启动调度
   */
  void run();

  /**
   * @brief 调度循环
   * 运行在scheduler_fiber中，负责调度和执行用户协程
   */
  void schedule_loop();

  std::string name_;                                  // 调度器名称
  int thread_count_;                                  // 线程数量
  std::vector<std::unique_ptr<std::thread>> threads_; // 线程池

  std::atomic<uint32_t> rr_enqueue_{0};
  std::atomic<size_t> pending_tasks_{0};

  // 启动屏障：确保 start() 返回前，所有 worker 的 work queue 已注册。
  mutable std::mutex start_mutex_;
  std::condition_variable start_cv_;
  std::atomic<int> registered_worker_queues_{0};

  std::atomic<bool> stopping_;           // 停止标志
  std::atomic<int> active_thread_count_; // 活跃线程数
  std::atomic<int> idle_thread_count_;   // 空闲线程数

  std::vector<std::atomic<WorkStealingQueue *>>
      work_queues_; // 队列指针注册表：0..thread_count_-1 为 worker

  // 可窃取队列提示位图：用于引导非随机的任务窃取选择。
  StealableQueueBitmap stealable_bitmap_;

  // 共享栈相关
  bool use_shared_stack_ = false; // 是否使用共享栈模式
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SCHEDULER_H_
