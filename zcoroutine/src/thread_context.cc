#include "thread_context.h"

#include <memory>

#include "context.h"
#include "fiber.h"
#include "fiber_pool.h"
#include "processor.h"
#include "shared_stack.h"
#include "zcoroutine_logger.h"
namespace zcoroutine {

constexpr int ThreadContext::kMaxCallStackDepth;

// 线程本地变量，存储当前线程的上下文
thread_local std::unique_ptr<ThreadContext> t_thread_context = nullptr;

ThreadContext *ThreadContext::get_current() {
  if (!t_thread_context) {
    t_thread_context = std::make_unique<ThreadContext>();
    // 初始化默认值
    t_thread_context->shared_stack_ctx_.stack_mode = StackMode::kIndependent;
  }
  return t_thread_context.get();
}

void ThreadContext::set_main_fiber(const Fiber::ptr &fiber) {
  auto *ctx = get_current();
  ctx->scheduler_ctx_.main_fiber = fiber;
  if (fiber) {
    ctx->scheduler_ctx_.call_stack_size = 0;

    // 将 main_fiber 压入调用栈
    if (ctx->scheduler_ctx_.call_stack_size < kMaxCallStackDepth) {
      ctx->scheduler_ctx_.call_stack[ctx->scheduler_ctx_.call_stack_size++] =
          fiber;
    }
    ctx->scheduler_ctx_.current_fiber = fiber; // 设置当前协程为 current fiber
  } else {
    ctx->scheduler_ctx_.call_stack_size = 0;
    ctx->scheduler_ctx_.current_fiber.reset();
  }
}

Fiber::ptr ThreadContext::get_main_fiber() {
  return get_current()->scheduler_ctx_.main_fiber.lock();
}

void ThreadContext::set_current_fiber(const Fiber::ptr &fiber) {
  get_current()->scheduler_ctx_.current_fiber = fiber;
}

Fiber::ptr ThreadContext::get_current_fiber() {
  return get_current()->scheduler_ctx_.current_fiber.lock();
}

void ThreadContext::set_scheduler_fiber(const Fiber::ptr &fiber) {
  get_current()->scheduler_ctx_.scheduler_fiber = fiber;
}

Fiber::ptr ThreadContext::get_scheduler_fiber() {
  return get_current()->scheduler_ctx_.scheduler_fiber.lock();
}

void ThreadContext::set_scheduler(Scheduler *scheduler) {
  get_current()->scheduler_ctx_.scheduler = scheduler;
}

Scheduler *ThreadContext::get_scheduler() {
  return get_current()->scheduler_ctx_.scheduler;
}

int ThreadContext::get_worker_id() {
  auto *ctx = get_current();
  return ctx->scheduler_ctx_.processor ? ctx->scheduler_ctx_.processor->id : -1;
}

void ThreadContext::set_processor(Processor *processor) {
  auto *ctx = get_current();
  ctx->scheduler_ctx_.processor = processor;
}

Processor *ThreadContext::get_processor() {
  return get_current()->scheduler_ctx_.processor;
}

void ThreadContext::set_stack_mode(StackMode mode) {
  get_current()->shared_stack_ctx_.stack_mode = mode;
}

StackMode ThreadContext::get_stack_mode() {
  return get_current()->shared_stack_ctx_.stack_mode;
}

void ThreadContext::set_shared_stack(
    std::unique_ptr<SharedStack> shared_stack) {
  get_current()->shared_stack_ctx_.shared_stack = std::move(shared_stack);
}

SharedStack *ThreadContext::get_shared_stack() {
  auto *ctx = get_current();
  // 如果是共享栈模式但未设置共享栈，自动创建
  if (ctx->shared_stack_ctx_.stack_mode == StackMode::kShared &&
      !ctx->shared_stack_ctx_.shared_stack) {
    auto &pool = FiberPool::get_instance();
    const size_t stack_size = pool.initialized()
                                  ? pool.shared_stack_size()
                                  : SharedStack::kDefaultStackSize;
    ctx->shared_stack_ctx_.shared_stack = std::make_unique<SharedStack>(
        SharedStack::kDefaultStackCount, stack_size);
  }
  return ctx->shared_stack_ctx_.shared_stack.get();
}

void ThreadContext::reset_shared_stack_config() {
  auto *ctx = get_current();
  ctx->shared_stack_ctx_.stack_mode = StackMode::kIndependent;
  ctx->shared_stack_ctx_.shared_stack = nullptr;
}

SwitchStack *ThreadContext::get_switch_stack() {
  auto *ctx = get_current();
  // 如果不存在则自动创建
  if (!ctx->shared_stack_ctx_.switch_stack) {
    ctx->shared_stack_ctx_.switch_stack = std::make_unique<SwitchStack>();
  }
  return ctx->shared_stack_ctx_.switch_stack.get();
}

Context *ThreadContext::get_switch_context() {
  auto *ctx = get_current();
  // 如果不存在则自动创建
  if (!ctx->shared_stack_ctx_.switch_context) {
    // 确保 switch_stack 已创建
    SwitchStack *switch_stack = get_switch_stack();

    ctx->shared_stack_ctx_.switch_context = std::make_unique<Context>();
    // 初始化切换上下文，使其运行在 switch_stack 上
    ctx->shared_stack_ctx_.switch_context->make_context(
        switch_stack->buffer(), switch_stack->size(), SwitchStack::switch_func);

    ZCOROUTINE_LOG_DEBUG(
        "ThreadContext: created switch_context on switch_stack");
  }
  return ctx->shared_stack_ctx_.switch_context.get();
}

void ThreadContext::set_pending_fiber(const Fiber::ptr &fiber) {
  get_current()->shared_stack_ctx_.pending_fiber = fiber;
}

Fiber::ptr ThreadContext::get_pending_fiber() {
  return get_current()->shared_stack_ctx_.pending_fiber.lock();
}

void ThreadContext::set_hook_enable(bool enable) {
  get_current()->hook_ctx_.hook_enable = enable;
}

bool ThreadContext::is_hook_enabled() {
  return get_current()->hook_ctx_.hook_enable;
}

void ThreadContext::push_call_stack(const Fiber::ptr &fiber) {
  auto *ctx = get_current();
  if (!fiber)
    return;
  if (ctx->scheduler_ctx_.call_stack_size < kMaxCallStackDepth) {
    ctx->scheduler_ctx_.call_stack[ctx->scheduler_ctx_.call_stack_size++] =
        fiber;
  } else {
    ZCOROUTINE_LOG_WARN("Call stack depth reached max {}, fiber={}",
                        kMaxCallStackDepth, fiber->name());
  }
}

Fiber::ptr ThreadContext::pop_call_stack() {
  auto *ctx = get_current();
  if (ctx->scheduler_ctx_.call_stack_size <= 0)
    return nullptr;
  auto f =
      ctx->scheduler_ctx_.call_stack[ctx->scheduler_ctx_.call_stack_size - 1]
          .lock();
  ctx->scheduler_ctx_.call_stack[ctx->scheduler_ctx_.call_stack_size - 1]
      .reset();
  ctx->scheduler_ctx_.call_stack_size--;
  return f;
}

Fiber::ptr ThreadContext::top_call_stack() {
  auto *ctx = get_current();
  if (ctx->scheduler_ctx_.call_stack_size <= 0)
    return nullptr;
  return ctx->scheduler_ctx_.call_stack[ctx->scheduler_ctx_.call_stack_size - 1]
      .lock();
}

int ThreadContext::call_stack_size() {
  return get_current()->scheduler_ctx_.call_stack_size;
}

Fiber::ptr ThreadContext::fiber_pool_pop(bool shared) {
  auto *ctx = get_current();
  auto &vec = shared ? ctx->fiber_pool_ctx_.shared_free
                     : ctx->fiber_pool_ctx_.independent_free;
  if (vec.empty()) {
    return nullptr;
  }
  Fiber::ptr fiber = std::move(vec.back());
  vec.pop_back();
  return fiber;
}

bool ThreadContext::fiber_pool_push(const Fiber::ptr &fiber,
                                    size_t per_thread_max_capacity) {
  if (!fiber) {
    return false;
  }

  auto *ctx = get_current();
  const bool shared = fiber->is_shared_stack();
  auto &vec = shared ? ctx->fiber_pool_ctx_.shared_free
                     : ctx->fiber_pool_ctx_.independent_free;

  if (per_thread_max_capacity > 0) {
    const size_t total = ctx->fiber_pool_ctx_.independent_free.size() +
                         ctx->fiber_pool_ctx_.shared_free.size();
    if (total >= per_thread_max_capacity) {
      return false;
    }
  }

  vec.push_back(fiber);
  return true;
}

size_t ThreadContext::fiber_pool_size() {
  auto *ctx = get_current();
  return ctx->fiber_pool_ctx_.independent_free.size() +
         ctx->fiber_pool_ctx_.shared_free.size();
}

void ThreadContext::fiber_pool_clear() {
  auto *ctx = get_current();
  ctx->fiber_pool_ctx_.independent_free.clear();
  ctx->fiber_pool_ctx_.shared_free.clear();
}

} // namespace zcoroutine
