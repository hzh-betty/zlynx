#include "zcoroutine/sched.h"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "zcoroutine/internal/processor.h"
#include "zcoroutine/internal/runtime_manager.h"

namespace zcoroutine {

// sched.cc 提供的是对外 API 薄封装：
// - 上层调用 init/go/yield/sleep_for 等接口。
// - 底层统一委托给 Runtime + Processor。

Scheduler::Scheduler(size_t scheduler_index) : scheduler_index_(scheduler_index) {}

void Scheduler::go(Task task) {
  // 指定投递到某个调度器线程，常用于亲和性或分片场景。
  Runtime::instance().submit_to(scheduler_index_, std::move(task));
}

void Scheduler::go(Closure* cb) {
  if (cb == nullptr) {
    return;
  }

  this->go([cb]() {
    std::unique_ptr<Closure> holder(cb);
    holder->run();
  });
}

int Scheduler::id() const { return static_cast<int>(scheduler_index_); }

void init(uint32_t scheduler_count) {
  // scheduler_count=0 表示按 CPU 核数自动选择。
  Runtime::instance().init(scheduler_count);
}

void co_stack_num(size_t stack_num) {
  Runtime::instance().set_stack_num(stack_num);
}

void co_stack_size(size_t stack_size) {
  Runtime::instance().set_stack_size(stack_size);
}

void co_stack_model(StackModel stack_model) {
  Runtime::instance().set_stack_model(stack_model);
}

void shutdown() { Runtime::instance().shutdown(); }

void go(Closure* cb) {
  if (cb == nullptr) {
    return;
  }

  go([cb]() {
    std::unique_ptr<Closure> holder(cb);
    holder->run();
  });
}

void go(Task task) {
  // 不指定调度器时走轮询分发。
  Runtime::instance().submit(std::move(task));
}

Scheduler* main_sched() { return Runtime::instance().main_scheduler(); }

Scheduler* next_sched() { return Runtime::instance().next_scheduler(); }

void stop_scheds() { Runtime::instance().shutdown(); }

void yield() {
  // 线程上下文没有当前 fiber：直接让出 OS 线程时间片。
  Processor* processor = current_processor();
  if (!processor || !processor->current_fiber()) {
    std::this_thread::yield();
    return;
  }
  // 协程上下文：让出执行权给同调度器中的其他就绪协程。
  processor->yield_current();
}

void sleep_for(uint32_t milliseconds) {
  // 线程上下文走阻塞睡眠；协程上下文走定时器挂起。
  Processor* processor = current_processor();
  if (!processor || !processor->current_fiber()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    return;
  }

  if (milliseconds == 0) {
    // zero-sleep 语义等价于主动让出执行权，无需进入定时器队列。
    processor->yield_current();
    return;
  }

  processor->prepare_wait_current();
  (void)processor->park_current_for(milliseconds);
}

void resume(void* fiber) {
  // 对外暴露的恢复入口，实际恢复逻辑在 Runtime::resume_external。
  Runtime::instance().resume_external(fiber);
}

void* current_coroutine() {
  // 返回逻辑句柄用于跨 API 传递，不暴露 Fiber 对象地址。
  Fiber::ptr fiber = current_fiber_shared();
  return Runtime::instance().external_handle(fiber.get());
}

int sched_id() {
  Processor* processor = current_processor();
  return processor ? processor->id() : -1;
}

int coroutine_id() {
  Fiber::ptr fiber = current_fiber_shared();
  return fiber ? fiber->id() : -1;
}

bool timeout() {
  // 仅在协程被超时路径唤醒后返回 true。
  Fiber::ptr fiber = current_fiber_shared();
  return fiber ? fiber->timed_out() : false;
}

bool in_coroutine() { return current_fiber_shared() != nullptr; }

size_t scheduler_count() { return Runtime::instance().scheduler_count(); }

}  // namespace zcoroutine
