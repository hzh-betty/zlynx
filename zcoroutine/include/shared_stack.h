#ifndef ZCOROUTINE_SHARED_STACK_H_
#define ZCOROUTINE_SHARED_STACK_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

// 前向声明
class Fiber;
class Context;

/**
 * @brief 栈模式枚举
 * 定义协程使用的栈模式
 */
enum class StackMode {
  kIndependent, // 独立栈模式（默认）：每个协程拥有独立的栈空间
  kShared // 共享栈模式：多个协程共享同一个栈空间，切换时保存/恢复栈内容
};

/**
 * @brief 共享栈缓冲区类
 * 表示一个可被多个协程共享的栈空间
 * 同一时刻只有一个协程可以占用此栈空间
 */
class SharedStackBuffer : public NonCopyable {
public:
  using ptr = std::shared_ptr<SharedStackBuffer>;

  /**
   * @brief 构造函数
   * @param stack_size 栈大小
   */
  explicit SharedStackBuffer(size_t stack_size);

  /**
   * @brief 析构函数
   */
  ~SharedStackBuffer();

  /**
   * @brief 获取栈缓冲区指针
   * @return 栈缓冲区起始地址
   */
  char *buffer() const { return stack_buffer_; }

  /**
   * @brief 获取栈顶指针（高地址端）
   * @return 栈顶地址
   */
  char *stack_top() const { return stack_bp_; }

  /**
   * @brief 获取栈大小
   * @return 栈大小
   */
  size_t size() const { return stack_size_; }

  /**
   * @brief 获取当前占用此栈的协程
   * @return 占用协程指针，如果无协程占用则返回nullptr
   * @note 使用原始指针避免循环引用
   */
  Fiber *occupy_fiber() const { return occupy_fiber_; }

  /**
   * @brief 设置占用此栈的协程
   * @param fiber 协程指针
   */
  void set_occupy_fiber(Fiber *fiber) { occupy_fiber_ = fiber; }

private:
  char *stack_buffer_ = nullptr; // 栈缓冲区起始地址（低地址）
  char *stack_bp_ = nullptr; // 栈顶地址（高地址，栈从高往低增长）
  size_t stack_size_ = 0;    // 栈大小
  Fiber *occupy_fiber_ = nullptr; // 当前占用此栈的协程（原始指针避免循环引用）
};

/**
 * @brief 共享栈池类
 * 管理一组共享栈缓冲区，提供轮询分配策略
 * 可以被多个协程共享使用
 */
class SharedStack : public NonCopyable {
public:
  using ptr = std::shared_ptr<SharedStack>;

  /**
   * @brief 默认共享栈大小：128KB
   */
  static constexpr size_t kDefaultStackSize = 128 * 1024;

  /**
   * @brief 默认共享栈数量
   */
  static constexpr int kDefaultStackCount = 10;

  /**
   * @brief 构造函数
   * @param count 共享栈缓冲区数量
   * @param stack_size 每个栈的大小
   */
  explicit SharedStack(int count = kDefaultStackCount,
                       size_t stack_size = kDefaultStackSize);

  /**
   * @brief 析构函数
   */
  ~SharedStack() = default;

  /**
   * @brief 获取一个共享栈缓冲区（轮询分配）
   * @return 共享栈缓冲区指针
   */
  SharedStackBuffer *allocate();

  /**
   * @brief 获取栈大小
   * @return 栈大小
   */
  size_t stack_size() const { return stack_size_; }

  /**
   * @brief 获取栈缓冲区数量
   * @return 缓冲区数量
   */
  int count() const { return count_; }

private:
  std::vector<std::unique_ptr<SharedStackBuffer>> stack_array_; // 栈缓冲区数组
  size_t stack_size_ = 0; // 每个栈的大小
  int count_ = 0;         // 栈缓冲区数量
  size_t index_ = 0;      // 轮询下标
};

/**
 * @brief 专用切换栈类
 * 用于协程切换时作为临时栈使用，避免共享栈切换时的栈覆盖问题
 * 每个线程拥有独立的SwitchStack
 */
class SwitchStack : public NonCopyable {
public:
  using ptr = std::unique_ptr<SwitchStack>;

  /**
   * @brief 默认切换栈大小：32KB（足够用于上下文切换）
   */
  static constexpr size_t kDefaultSwitchStackSize = 32 * 1024;

  /**
   * @brief 构造函数
   * @param stack_size 栈大小
   */
  explicit SwitchStack(size_t stack_size = kDefaultSwitchStackSize);

  /**
   * @brief 析构函数
   */
  ~SwitchStack();

  /**
   * @brief 获取栈缓冲区指针
   * @return 栈缓冲区起始地址
   */
  char *buffer() const { return stack_buffer_; }

  /**
   * @brief 获取栈顶指针（高地址端）
   * @return 栈顶地址
   */
  char *stack_top() const { return stack_bp_; }

  /**
   * @brief 获取栈大小
   * @return 栈大小
   */
  size_t size() const { return stack_size_; }

  /**
   * @brief 切换函数 - 运行在 switch stack 上
   *
   * 这个函数在专用切换栈上执行，负责：
   * 1. 从当前协程的 context 中获取 rsp（已由 swapcontext 保存）
   * 2. 保存当前协程的栈内容（如果是共享栈）
   * 3. 恢复目标协程的栈内容（如果是共享栈）
   * 4. 切换到目标协程
   *
   * 整个过程不使用任何 magic number，所有栈操作都在独立的 switch stack 上进行
   */
  static void switch_func();

private:
  char *stack_buffer_ = nullptr; // 栈缓冲区起始地址（低地址）
  char *stack_bp_ = nullptr; // 栈顶地址（高地址，栈从高往低增长）
  size_t stack_size_ = 0;    // 栈大小
};

/**
 * @brief 协程栈上下文类
 * 封装协程共享栈相关的所有成员
 */
class SharedContext {
public:
  SharedContext() = default;
  ~SharedContext();

  /**
   * @brief 初始化为共享栈模式
   * @param buffer 共享栈缓冲区
   */
  void init_shared(SharedStackBuffer *buffer);

  /**
   * @brief 检查是否使用共享栈
   */
  bool is_shared_stack() const { return is_shared_stack_; }

  /**
   * @brief 获取共享栈缓冲区
   */
  SharedStackBuffer *shared_stack_buffer() const {
    return shared_stack_buffer_;
  }

  /**
   * @brief 获取保存缓冲区
   */
  char *save_buffer() const { return save_buffer_; }

  /**
   * @brief 获取保存的栈大小
   */
  size_t save_size() const { return save_size_; }

  /**
   * @brief 保存栈内容到save_buffer
   * 用于共享栈切换时保存当前协程的栈内容
   * @param stack_sp 栈指针（从 ucontext 中获取）
   * @note 必须在 switch stack 上调用，不需要 magic number
   */
  void save_stack_buffer(void *stack_sp);

  /**
   * @brief 恢复栈内容
   * 用于共享栈切换时恢复协程的栈内容
   * @note 必须在 switch stack 上调用
   */
  void restore_stack_buffer();

  /**
   * @brief 重置共享栈状态（协程重置时调用）
   */
  void reset();

  /**
   * @brief 清除占用标记（协程析构或终止时调用）
   * @param owner 所属的Fiber指针，用于检查是否是占用者
   */
  void clear_occupy(const Fiber *owner);

  /**
   * @brief 获取保存的栈指针（用于恢复）
   */
  void *saved_stack_sp() const { return saved_stack_sp_; }

private:
  // 缓存优化：热数据（频繁访问）放在一起，对齐到缓存行
  // 第一缓存行：高频访问的状态和指针
  alignas(64) char *save_buffer_ = nullptr; // 栈内容保存缓冲区 - 每次切换都访问
  void *saved_stack_sp_ = nullptr; // 保存时的栈指针 - 每次切换都访问
  size_t save_size_ = 0;           // 保存的栈大小 - 每次切换都访问
  size_t save_buffer_capacity_ = 0;                  // 缓冲区容量
  SharedStackBuffer *shared_stack_buffer_ = nullptr; // 共享栈缓冲区
  bool is_shared_stack_ = false;                     // 是否使用共享栈
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SHARED_STACK_H_
