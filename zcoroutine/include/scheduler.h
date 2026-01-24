#ifndef ZCOROUTINE_SCHEDULER_H_
#define ZCOROUTINE_SCHEDULER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include "fiber.h"
#include "task_queue.h" // for Task
#include "thread_context.h"
#include "work_stealing_thread_pool.h"

namespace zcoroutine {

class WorkStealingQueue;

/**
 * @brief 调度器类
 * 基于线程池的M:N调度模型
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

  std::string name_; // 调度器名称

  WorkStealingThreadPool pool_;

  std::atomic<size_t> pending_tasks_{0};
  std::atomic<bool> stopping_; // 停止标志

  // 共享栈相关
  bool use_shared_stack_ = false; // 是否使用共享栈模式
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SCHEDULER_H_
