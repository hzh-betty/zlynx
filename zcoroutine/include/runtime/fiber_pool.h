#ifndef ZCOROUTINE_FIBER_POOL_H_
#define ZCOROUTINE_FIBER_POOL_H_

#include <atomic>
#include <functional>

#include "runtime/fiber.h"
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 全局协程池
 *
 * 用于复用已终止的协程对象，
 * 避免频繁创建和销毁协程带来的开销。
 */
class FiberPool : public NonCopyable {
public:
  /**
   * @brief 获取全局单例
   * @return 协程池单例引用
   */
  static FiberPool &get_instance();

  /**
   * @brief 初始化协程池全局配置（全局一次）
   * @param independent_stack_size 所有线程支持的独立栈大小
   * @param shared_stack_size 所有线程支持的共享栈大小
   * @param per_thread_max_capacity 每线程最大缓存协程数（0 表示不限制）
   *
   * @note 必须在使用池之前调用；重复调用将被忽略
   */
  void init(size_t independent_stack_size = StackAllocator::kDefaultStackSize,
            size_t shared_stack_size = SharedStack::kDefaultStackSize,
            size_t per_thread_max_capacity = 1000);

  bool initialized() const;

  size_t independent_stack_size() const;
  size_t shared_stack_size() const;
  size_t per_thread_max_capacity() const;

  /**
   * @brief 从池中获取或创建一个协程
   * @param func 协程执行函数
   * @param stack_size 栈大小，默认128KB
   * @param name 协程名称前缀，默认为空
   * @param use_shared_stack 是否使用共享栈，默认false
   * @return 协程智能指针
   */
  Fiber::ptr get_fiber(std::function<void()> func,
                       size_t stack_size = StackAllocator::kDefaultStackSize,
                       const std::string &name = "",
                       bool use_shared_stack = false);

  /**
   * @brief 归还协程到池中
   * @param fiber 要归还的协程指针
   * @return true表示成功归还，false表示池已满直接丢弃
   *
   * @note 只有处于Terminated状态的协程才能归还
   * @note 通常不需要手动调用，协程执行完毕后会自动归还
   */
  bool return_fiber(const Fiber::ptr &fiber);

private:
  FiberPool();
  ~FiberPool() = default;

  // 全局配置（由 FiberPool 单例持有，所有线程共享）
  std::atomic<bool> initialized_{false};
  std::atomic<size_t> independent_stack_size_{StackAllocator::kDefaultStackSize};
  std::atomic<size_t> shared_stack_size_{SharedStack::kDefaultStackSize};
  std::atomic<size_t> per_thread_max_capacity_{1000};
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_POOL_H_
