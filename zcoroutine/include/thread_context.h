#ifndef ZCOROUTINE_THREAD_CONTEXT_H_
#define ZCOROUTINE_THREAD_CONTEXT_H_

#include "fiber.h"
#include <array>
#include <memory>
#include <vector>

namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;
class SharedStack;
class SwitchStack;
class Context;
class WorkStealingQueue;

// 栈模式枚举前向声明
enum class StackMode;

/**
 * @brief 调度器上下文
 * 管理调度器相关的状态，如主协程、当前协程、调度器协程等
 */
struct SchedulerContext {
  std::weak_ptr<Fiber> main_fiber;      // 主协程（线程入口协程）
  std::weak_ptr<Fiber> current_fiber;   // 当前执行的协程
  std::weak_ptr<Fiber> scheduler_fiber; // 调度器协程
  Scheduler *scheduler = nullptr;       // 当前调度器

  int worker_id = -1; // 当前线程的 worker id
  WorkStealingQueue *work_queue =
      nullptr; // 当前线程的 work-stealing 队列（可注入）
  std::unique_ptr<WorkStealingQueue>
      owned_work_queue; // 非 worker/未注入时的兜底所有权

  static constexpr int kMaxCallStackDepth = 128;
  std::array<std::weak_ptr<Fiber>, kMaxCallStackDepth> call_stack{};
  int call_stack_size = 0;
};

/**
 * @brief 共享栈上下文
 * 管理共享栈模式下的栈状态、切换栈等
 */
struct SharedStackContext {
  StackMode stack_mode;                                // 当前线程的栈模式
  std::unique_ptr<SharedStack> shared_stack = nullptr; // 当前线程的共享栈
  std::unique_ptr<SwitchStack> switch_stack = nullptr; // 专用切换栈
  std::unique_ptr<Context> switch_context =
      nullptr;                        // 切换上下文（运行在 switch_stack_ 上）
  std::weak_ptr<Fiber> pending_fiber; // 待切换的目标协程
};

/**
 * @brief Hook上下文
 * 管理系统调用Hook的状态
 */
struct HookContext {
  bool hook_enable = false; // Hook启用标志
};

/**
 * @brief 协程池（线程本地）上下文
 * 分别维护独立栈与共享栈的空闲协程列表。
 *
 * 约束：
 * - 只允许本线程访问（由 ThreadContext 的 thread_local 保证）
 * - get/return 均为 O(1) push/pop
 */
struct FiberPoolContext {
  std::vector<std::shared_ptr<Fiber>> independent_free;
  std::vector<std::shared_ptr<Fiber>> shared_free;
};

/**
 * @brief 线程上下文类
 * 集中管理线程本地状态，包括主协程、当前协程、调度器协程、调度器指针等
 * 使用thread_local变量存储，每个线程独立
 *
 * 协程切换层次结构：
 *   main_fiber <---> scheduler_fiber <---> user_fiber
 *   - main_fiber: 线程入口协程，保存线程的原始上下文
 *   - scheduler_fiber: 调度器协程，运行Scheduler::run()
 *   - user_fiber: 用户协程，执行用户任务
 */
class ThreadContext {
public:
  static constexpr int kMaxCallStackDepth =
      SchedulerContext::kMaxCallStackDepth;
  /**
   * @brief 获取当前线程的上下文
   * @return 当前线程的ThreadContext指针，如果不存在则创建
   */
  static ThreadContext *get_current();

  /**
   * @brief 设置主协程
   * @param fiber 主协程指针
   */
  static void set_main_fiber(const Fiber::ptr &fiber);

  /**
   * @brief 获取主协程
   * @return 主协程指针
   */
  static Fiber::ptr get_main_fiber();

  /**
   * @brief 设置当前执行的协程
   * @param fiber 协程指针
   */
  static void set_current_fiber(const Fiber::ptr &fiber);

  /**
   * @brief 获取当前执行的协程
   * @return 当前协程指针
   */
  static Fiber::ptr get_current_fiber();

  /**
   * @brief 设置调度器协程
   * @param fiber 调度器协程指针
   */
  static void set_scheduler_fiber(const Fiber::ptr &fiber);

  /**
   * @brief 获取调度器协程
   * @return 调度器协程指针
   */
  static Fiber::ptr get_scheduler_fiber();

  /**
   * @brief 设置当前调度器
   * @param scheduler 调度器指针
   */
  static void set_scheduler(Scheduler *scheduler);

  /**
   * @brief 获取当前调度器
   * @return 调度器指针
   */
  static Scheduler *get_scheduler();

  /**
   * @brief 设置/获取当前线程的 worker id
   */
  static void set_worker_id(int id);
  static int get_worker_id();

  /**
   * @brief 获取当前线程的 work-stealing 队列（按需创建，仅线程本地 owner）
   */
  static WorkStealingQueue *get_work_queue();

  /**
   * @brief 注入当前线程的 work-stealing 队列（通常由线程池/Processor
   * 创建并拥有）
   */
  static void set_work_queue(WorkStealingQueue *queue);

  /**
   * @brief 设置当前线程的栈模式
   * @param mode 栈模式
   */
  static void set_stack_mode(StackMode mode);

  /**
   * @brief 获取当前线程的栈模式
   * @return 栈模式
   */
  static StackMode get_stack_mode();

  /**
   * @brief 设置当前线程的共享栈
   * @param shared_stack 共享栈指针
   */
  static void set_shared_stack(std::unique_ptr<SharedStack> shared_stack);

  /**
   * @brief 获取当前线程的共享栈
   * @return 共享栈指针，如果未设置且模式为共享栈则自动创建
   */
  static SharedStack *get_shared_stack();

  /**
   * @brief 重置共享栈配置为默认值（独立栈模式）
   */
  static void reset_shared_stack_config();

  /**
   * @brief 获取当前线程的专用切换栈
   * @return 切换栈指针，如果不存在则自动创建
   */
  static SwitchStack *get_switch_stack();

  /**
   * @brief 获取当前线程的切换上下文（运行在 switch stack 上）
   * @return 切换上下文指针，如果不存在则自动创建
   */
  static Context *get_switch_context();

  /**
   * @brief 设置待切换的目标协程
   * @param fiber 目标协程
   */
  static void set_pending_fiber(const Fiber::ptr &fiber);

  /**
   * @brief 获取待切换的目标协程
   * @return 目标协程
   */
  static Fiber::ptr get_pending_fiber();

  /**
   * @brief 设置Hook启用状态
   * @param enable true表示启用，false表示禁用
   */
  static void set_hook_enable(bool enable);

  /**
   * @brief 获取Hook启用状态
   * @return true表示启用，false表示禁用
   */
  static bool is_hook_enabled();

  static void push_call_stack(const Fiber::ptr &fiber);
  static Fiber::ptr pop_call_stack();
  static Fiber::ptr top_call_stack();
  static int call_stack_size();

  /**
   * @brief 从协程池中弹出一个协程
   * @param shared true表示弹出共享栈协程，false表示独立栈协程
   * @return 协程指针，如果池为空则返回nullptr
   */
  static Fiber::ptr fiber_pool_pop(bool shared);
  static bool fiber_pool_push(const Fiber::ptr &fiber,
                              size_t per_thread_max_capacity);
  static size_t fiber_pool_size();
  static void fiber_pool_clear();

private:
  // 调度器上下文
  SchedulerContext scheduler_ctx_;

  // 共享栈上下文
  SharedStackContext shared_stack_ctx_{};

  // Hook上下文
  HookContext hook_ctx_;

  // 协程池上下文
  FiberPoolContext fiber_pool_ctx_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_THREAD_CONTEXT_H_
