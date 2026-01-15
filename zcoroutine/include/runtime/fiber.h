#ifndef ZCOROUTINE_FIBER_H_
#define ZCOROUTINE_FIBER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "runtime/context.h"
#include "runtime/shared_stack.h"
#include "runtime/stack_allocator.h"
namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;

/**
 * @brief 协程类
 * 管理协程的生命周期、状态和上下文切换
 */
class Fiber : public std::enable_shared_from_this<Fiber>, public NonCopyable {
public:
  using ptr = std::shared_ptr<Fiber>;

  /**
   * @brief 协程状态枚举
   */
  enum class State {
    kReady,     // 就绪态，等待调度
    kRunning,   // 运行态，正在执行
    kSuspended, // 挂起态，主动让出CPU
    kTerminated // 终止态，执行完毕
  };

  /**
   * @brief 将State枚举转换为字符串
   * @param state 状态类型
   * @return 状态类型的字符串表示
   */
  static const char *state_to_string(State state) {
    switch (state) {
    case State::kReady:
      return "READY";
    case State::kRunning:
      return "RUNNING";
    case State::kSuspended:
      return "SUSPENDED";
    case State::kTerminated:
      return "TERMINATED";
    default:
      return "UNKNOWN";
    }
  }

  /**
   * @brief 构造函数
   * @param func 协程执行函数
   * @param stack_size 栈大小，默认128KB
   * @param name 协程名称，默认为空（自动生成fiber_id）
   * @param use_shared_stack 是否强制使用共享栈（忽略全局配置）
   */
  explicit Fiber(std::function<void()> func,
                 size_t stack_size = StackAllocator::kDefaultStackSize,
                 const std::string &name = "", bool use_shared_stack = false);

  /**
   * @brief 构造函数（使用指定共享栈）
   * @param func 协程执行函数
   * @param shared_stack 共享栈指针
   * @param name 协程名称，默认为空（自动生成fiber_id）
   */
  Fiber(std::function<void()> func, SharedStack *shared_stack,
        const std::string &name = "");

  /**
   * @brief 移动构造函数
   * @param other 右值引用对象
   */
  Fiber(Fiber &&other) noexcept;

  /**
   * @brief 移动复制函数
   * @param other 右值引用对象
   */
  Fiber &operator=(Fiber &&other) noexcept;

  /**
   * @brief 析构函数
   */
  ~Fiber();

  /**
   * @brief 恢复协程执行
   * 从当前协程切换到此协程
   */
  void resume();

  /**
   * @brief 挂起协程
   * 将控制权交还给调用者或主协程
   */
  static void yield();

  /**
   * @brief 重置协程
   * @param func 新的执行函数
   * 用于协程池复用协程对象
   */
  void reset(std::function<void()> func);

  /**
   * @brief 获取协程名称
   * @return 协程名称（格式：name_id或fiber_id）
   */
  std::string name() const { return name_; }

  /**
   * @brief 获取协程ID
   * @return 协程全局唯一ID
   */
  uint64_t id() const { return id_; }

  /**
   * @brief 获取协程状态
   * @return 当前状态
   */
  State state() const { return state_; }

  /**
   * @brief 检查是否使用共享栈
   * @return true表示使用共享栈，false表示使用独立栈
   */
  bool is_shared_stack() const {
    return shared_ctx_ && shared_ctx_->is_shared_stack();
  }

  /**
   * @brief 获取栈模式
   * @return 当前使用的栈模式
   */
  StackMode stack_mode() const {
    return shared_ctx_ && shared_ctx_->is_shared_stack()
               ? StackMode::kShared
               : StackMode::kIndependent;
  }

  /**
   * @brief 获取协程实际使用的栈大小
   * @return 栈大小（字节）
   */
  size_t stack_size() const { return stack_size_; }

  /**
   * @brief 获取栈上下文（共享栈相关）
   * @return 栈上下文指针
   */
  SharedContext *get_shared_context() { return shared_ctx_.get(); }
  const SharedContext *get_shared_context() const { return shared_ctx_.get(); }

  /**
   * @brief 获取上下文对象
   * @return 上下文指针
   * @note 用于 switch_func 读取已保存的栈指针
   */
  Context *context() { return context_.get(); }
  const Context *context() const { return context_.get(); }

  /**
   * @brief 协程主函数（静态）
   * 在协程上下文中执行
   */
  static void main_func();

  /**
   * @brief 获取当前执行的协程
   * @return 当前协程指针
   */
  static Fiber::ptr get_this();

  /**
   * @brief 设置当前协程
   * @param fiber 协程指针
   */
  static void set_this(const Fiber::ptr &fiber);

private:
  // Scheduler需要访问私有构造函数创建main_fiber
  friend class Scheduler;

  /**
   * @brief 主协程构造函数（私有）
   * 用于创建线程的主协程
   */
  Fiber();

  /**
   * @brief 确定切换目标协程(切换到scheduler_fiber或main_fiber)
   */
  void confirm_switch_target();

  /**
   * @brief 统一的协程切换函数（类似libco的co_swap）
   * 使用专用切换栈（switch stack）执行上下文切换
   * @param curr 当前协程
   * @param target 目标协程
   */
  static void co_swap(const Fiber::ptr &curr, const Fiber::ptr &target);

  // 缓存优化：将热数据（频繁访问）放在前面
  State state_ = State::kReady; // 协程状态 - 最常访问
  uint64_t id_ = 0;             // 协程唯一ID - 常访问
  void *stack_ptr_ = nullptr;   // 栈指针 - 常访问
  size_t stack_size_ = 0;       // 栈大小

  std::unique_ptr<Context> context_; // 上下文对象
  std::string name_;                 // 协程名称 - 主要用于日志

  std::function<void()> callback_; // 协程执行函数
  std::exception_ptr exception_;   // 协程异常指针

  // 共享栈上下文（封装所有共享栈相关成员）
  std::unique_ptr<SharedContext> shared_ctx_ = nullptr;

  // 全局协程计数器（线程安全）
  static std::atomic<uint64_t> s_fiber_count_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_H_
