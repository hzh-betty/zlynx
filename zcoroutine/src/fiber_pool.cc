#include "fiber_pool.h"

#include <mutex>

#include "thread_context.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

FiberPool::FiberPool() { ZCOROUTINE_LOG_INFO("FiberPool initialized"); }

FiberPool &FiberPool::get_instance() {
  static FiberPool instance;
  return instance;
}

void FiberPool::init(size_t independent_stack_size, size_t shared_stack_size,
                     size_t per_thread_max_capacity) {
  static std::mutex init_mutex;
  std::lock_guard<std::mutex> lock(init_mutex);
  if (initialized_.load(std::memory_order_acquire)) {
    return;
  }

  if (independent_stack_size == 0 || shared_stack_size == 0) {
    ZCOROUTINE_LOG_FATAL("FiberPool::init: stack_size must be > 0");
    abort();
  }

  // 设置全局配置
  independent_stack_size_.store(independent_stack_size,
                                std::memory_order_release);
  shared_stack_size_.store(shared_stack_size, std::memory_order_release);
  per_thread_max_capacity_.store(per_thread_max_capacity,
                                 std::memory_order_release);
  initialized_.store(true, std::memory_order_release);

  ZCOROUTINE_LOG_INFO(
      "FiberPool::init: independent_stack_size={}, shared_stack_size={}, "
      "per_thread_max_capacity={}",
      independent_stack_size, shared_stack_size, per_thread_max_capacity);
}

bool FiberPool::initialized() const {
  return initialized_.load(std::memory_order_acquire);
}

size_t FiberPool::independent_stack_size() const {
  return independent_stack_size_.load(std::memory_order_acquire);
}

size_t FiberPool::shared_stack_size() const {
  return shared_stack_size_.load(std::memory_order_acquire);
}

size_t FiberPool::per_thread_max_capacity() const {
  return per_thread_max_capacity_.load(std::memory_order_acquire);
}

Fiber::ptr FiberPool::get_fiber(std::function<void()> func, size_t stack_size,
                                const std::string &name,
                                bool use_shared_stack) {

  const bool wants_shared =
      use_shared_stack ||
      (ThreadContext::get_stack_mode() == StackMode::kShared);

  // 未 init：直接创建（不池化）
  if (!initialized()) {
    ZCOROUTINE_LOG_WARN(
        "FiberPool::get_fiber: not initialized, create directly");
    return std::make_shared<Fiber>(std::move(func), stack_size, name,
                                   wants_shared);
  }

  const size_t supported_stack_size =
      wants_shared ? shared_stack_size() : independent_stack_size();

  // 最多尝试复用一个
  Fiber::ptr fiber = ThreadContext::fiber_pool_pop(wants_shared);
  if (fiber) {
    // 若请求更大，复用失败：放回并直接创建新的
    if (fiber->stack_size() < stack_size) {
      (void)ThreadContext::fiber_pool_push(fiber, per_thread_max_capacity());
      return std::make_shared<Fiber>(std::move(func), stack_size, name,
                                     wants_shared);
    }
    fiber->reset(std::move(func));
    return fiber;
  }

  // 无可复用：若请求小于等于支持栈大小，创建支持栈大小；否则创建请求栈大小
  const size_t create_stack_size =
      (stack_size <= supported_stack_size) ? supported_stack_size : stack_size;

  return std::make_shared<Fiber>(std::move(func), create_stack_size, name,
                                 wants_shared);
}

bool FiberPool::return_fiber(const Fiber::ptr &fiber) {
  if (!fiber) {
    ZCOROUTINE_LOG_WARN("FiberPool::return_fiber: null fiber");
    return false;
  }

  if (!initialized()) {
    return false;
  }

  // 只有终止状态的协程才能归还
  if (fiber->state() != Fiber::State::kTerminated) {
    ZCOROUTINE_LOG_WARN("FiberPool::return_fiber: fiber not terminated, "
                        "fiber_id={}, state={}",
                        fiber->id(), Fiber::state_to_string(fiber->state()));
    return false;
  }

  const bool shared = fiber->is_shared_stack();
  const size_t supported_stack_size =
      shared ? shared_stack_size() : independent_stack_size();

  // 按规则：fiber 实际栈大小不足（< supported）直接拒绝
  if (fiber->stack_size() < supported_stack_size) {
    return false;
  }

  return ThreadContext::fiber_pool_push(fiber, per_thread_max_capacity());
}

} // namespace zcoroutine
