#include "zcoroutine/internal/processor.h"

#include <errno.h>
#include <sys/epoll.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "zcoroutine/internal/epoller.h"
#include "zcoroutine/internal/runtime_manager.h"
#include "zcoroutine/log.h"

namespace zcoroutine {

// Processor 是“单个调度线程”的核心执行体：
// - 接收 Task 并转化为 Fiber。
// - 维护就绪队列、IO 等待与定时器。
// - 负责 Fiber 上下文切换和共享栈快照恢复。

namespace {

thread_local Processor* tls_processor = nullptr;

}  // namespace

Processor::Processor(int id, size_t stack_size)
  : Processor(id, stack_size, kSharedStackGroupSize, StackModel::kShared) {}

Processor::Processor(int id, size_t stack_size, size_t shared_stack_num, StackModel stack_model)
    : id_(id),
      stack_size_(stack_size),
    stack_model_(stack_model),
      running_(false),
      worker_(),
      ready_size_(0),
      cpu_time_ns_(0),
      ema_loop_ns_(0),
      run_queue_mutex_(),
      run_queue_(),
      steal_queue_(),
      fiber_pool_(4096),
      next_stack_slot_(0),
      snapshot_pool_(),
      steal_probe_cursor_(0),
      timer_queue_(),
      poller_(create_default_poller()),
        shared_stacks_(stack_model == StackModel::kShared
                 ? (shared_stack_num == 0 ? 1 : shared_stack_num)
                 : 0,
               stack_size),
      scheduler_context_(),
      current_fiber_() {}

Processor::~Processor() {
  stop();
  join();

  current_fiber_.reset();
  fiber_pool_.clear();
}

void Processor::start() {
  running_.store(true, std::memory_order_release);
  worker_ = std::thread(&Processor::run_loop, this);
  ZCOROUTINE_LOG_INFO("processor started, sched_id={}, stack_size={}", id_, stack_size_);
}

void Processor::stop() {
  running_.store(false, std::memory_order_release);
  wake_loop();
  ZCOROUTINE_LOG_INFO("processor stop requested, sched_id={}", id_);
}

void Processor::join() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void Processor::enqueue_task(Task task) {
  steal_queue_.push(std::move(task));
  ZCOROUTINE_LOG_DEBUG("task enqueued, sched_id={}, pending_tasks={}", id_, steal_queue_.size());
  wake_loop();
}

void Processor::enqueue_ready(Fiber::ptr fiber) {
  if (!fiber) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(run_queue_mutex_);
    run_queue_.push_back(std::move(fiber));
    ready_size_.fetch_add(1, std::memory_order_relaxed);
    ZCOROUTINE_LOG_DEBUG("fiber ready enqueued, sched_id={}, ready_size={}", id_,
                         run_queue_.size());
  }

  wake_loop();
}

size_t Processor::steal_tasks(std::deque<Task>* tasks, size_t max_steal, size_t min_reserve) {
  const size_t count = steal_queue_.steal(tasks, max_steal, min_reserve);
  ZCOROUTINE_LOG_DEBUG("tasks stolen from sched_id={}, stolen={}, remaining_tasks={}", id_, count,
                       steal_queue_.size());
  return count;
}

uint32_t Processor::pending_task_count() const {
  return static_cast<uint32_t>(steal_queue_.size());
}

int Processor::id() const { return id_; }

Fiber::ptr Processor::current_fiber() const { return current_fiber_; }

ucontext_t* Processor::scheduler_context() { return scheduler_context_.get(); }

void Processor::yield_current() {
  if (!current_fiber_) {
    return;
  }

  current_fiber_->mark_ready();
  Context::swap_context(current_fiber_->context(), &scheduler_context_);
}

void Processor::prepare_wait_current() {
  if (!current_fiber_) {
    return;
  }

  current_fiber_->mark_waiting();
}

bool Processor::park_current() {
  if (!current_fiber_) {
    return false;
  }

  // 切回调度上下文，等待 IO/Timer 或其他路径唤醒后再恢复。
  Context::swap_context(current_fiber_->context(), &scheduler_context_);
  ZCOROUTINE_LOG_DEBUG("fiber resumed from park, sched_id={}, fiber_id={}, timed_out={}", id_,
                       current_fiber_->id(), current_fiber_->timed_out());
  return !current_fiber_->timed_out();
}

bool Processor::park_current_for(uint32_t milliseconds) {
  if (!current_fiber_) {
    return false;
  }

  if (milliseconds == kInfiniteTimeoutMs) {
    return park_current();
  }

  // 为当前等待协程挂一个超时回调，超时后尝试把协程恢复为 ready。
  Fiber::ptr waiting = current_fiber_;
  std::shared_ptr<TimerToken> token = add_timer(milliseconds, [waiting]() {
    resume_fiber(waiting, true);
  });

  // 协程被正常事件唤醒或超时回调唤醒后都会返回这里。
  const bool ok = park_current();
  token->cancelled.store(true, std::memory_order_release);
  return ok;
}

std::shared_ptr<TimerToken> Processor::add_timer(uint32_t milliseconds,
                                                 std::function<void()> callback) {
  std::shared_ptr<TimerToken> token = timer_queue_.add_timer(milliseconds, std::move(callback));
  ZCOROUTINE_LOG_DEBUG("timer added, sched_id={}, delay_ms={}", id_, milliseconds);
  wake_loop();
  return token;
}

bool Processor::wait_fd(int fd, uint32_t events, uint32_t milliseconds) {
  if (!current_fiber_) {
    ZCOROUTINE_LOG_WARN("wait_fd called without current fiber, sched_id={}, fd={}", id_, fd);
    return false;
  }

  ZCOROUTINE_LOG_DEBUG("wait_fd start, sched_id={}, fiber_id={}, fd={}, events={}, timeout_ms={}", id_,
                       current_fiber_->id(), fd, events, milliseconds);

  std::shared_ptr<IoWaiter> waiter = std::make_shared<IoWaiter>();
  waiter->fd = fd;
  waiter->events = events & (EPOLLIN | EPOLLOUT);
  waiter->fiber = current_fiber_;
  waiter->timer = nullptr;
  waiter->active.store(true, std::memory_order_release);

  if (waiter->events == 0) {
    errno = EINVAL;
    return false;
  }

  if (!poller_ || !poller_->register_waiter(waiter)) {
    ZCOROUTINE_LOG_ERROR("epoll add/mod failed, sched_id={}, fd={}, events={}, errno={}", id_, fd,
                         events, errno);
    waiter->active.store(false, std::memory_order_release);
    return false;
  }

  prepare_wait_current();

  if (milliseconds != kInfiniteTimeoutMs) {
    // timeout 回调与 IO 回调通过 waiter->active 竞争，只有一个路径生效。
    waiter->timer = add_timer(milliseconds, [this, waiter]() {
      if (!waiter->active.exchange(false, std::memory_order_acq_rel)) {
        return;
      }
      if (poller_) {
        poller_->unregister_waiter(waiter);
      }
      if (Fiber::ptr fiber = waiter->fiber.lock()) {
        ZCOROUTINE_LOG_DEBUG("wait_fd timeout, sched_id={}, fd={}, fiber_id={}", id_, waiter->fd,
                             fiber->id());
        resume_fiber(fiber, true);
      }
    });
  }

  const bool ok = park_current();

  waiter->active.store(false, std::memory_order_release);
  if (waiter->timer) {
    waiter->timer->cancelled.store(true, std::memory_order_release);
  }
  if (poller_) {
    poller_->unregister_waiter(waiter);
  }

  if (!ok) {
    ZCOROUTINE_LOG_DEBUG("wait_fd wake failed or timeout, sched_id={}, fd={}, timeout_ms={}", id_, fd,
                         milliseconds);
  }

  return ok;
}

void* Processor::shared_stack_data(size_t stack_slot) {
  return shared_stacks_.data(stack_slot);
}

size_t Processor::shared_stack_size(size_t stack_slot) const {
  return shared_stacks_.size(stack_slot);
}

size_t Processor::shared_stack_count() const { return shared_stacks_.count(); }

StackModel Processor::stack_model() const { return stack_model_; }

uint32_t Processor::queue_load() const {
  return ready_size_.load(std::memory_order_relaxed) + static_cast<uint32_t>(steal_queue_.size());
}

uint64_t Processor::cpu_time_ns() const { return cpu_time_ns_.load(std::memory_order_relaxed); }

uint64_t Processor::load_score() const {
  const uint64_t queue_component = static_cast<uint64_t>(queue_load()) * 1000000ULL;
  const uint64_t cpu_component = ema_loop_ns_.load(std::memory_order_relaxed) / 1000ULL;
  return queue_component + cpu_component;
}

void Processor::enqueue_stolen_tasks(std::deque<Task>* tasks) {
  steal_queue_.append(tasks);
}

char* Processor::acquire_snapshot_buffer(size_t required_size, size_t* capacity,
                                         uint8_t* bucket_index) {
  return snapshot_pool_.acquire(required_size, capacity, bucket_index);
}

void Processor::release_snapshot_buffer(char* buffer, uint8_t bucket_index, size_t capacity) {
  snapshot_pool_.release(buffer, bucket_index, capacity);
}

void Processor::run_loop() {
  set_current_processor(this);
  ZCOROUTINE_LOG_INFO("processor loop start, sched_id={}", id_);

  if (!poller_ || !poller_->start()) {
    running_.store(false, std::memory_order_release);
  }

  while (running_.load(std::memory_order_acquire)) {
    const auto loop_begin = std::chrono::steady_clock::now();

    // 先消化新任务和就绪队列，尽量降低调度延迟。
    drain_new_tasks();
    run_ready_tasks();

    // 再处理定时器，确保超时路径及时生效。
    process_timers();
    run_ready_tasks();

    if (!has_ready_tasks()) {
      wait_io_events_when_idle();
      steal_tasks_when_idle();
    }

    // 统计本轮循环的 CPU 时间，供负载评分使用。这里的时间包含了 run_ready_tasks 中执行任务的时间，因此能反映实际负载。
    const auto loop_end = std::chrono::steady_clock::now();
    const uint64_t loop_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end - loop_begin)
                                  .count());
    update_load_metrics(loop_ns);
  }

  if (poller_) {
    poller_->stop();
  }

  set_current_processor(nullptr);
  ZCOROUTINE_LOG_INFO("processor loop stop, sched_id={}", id_);
}

bool Processor::has_ready_tasks() const {
  std::lock_guard<std::mutex> lock(run_queue_mutex_);
  return !run_queue_.empty();
}

void Processor::wait_io_events_when_idle() {
  if (!poller_) {
    return;
  }

  const int timeout_ms = next_timeout_ms();
  // 没有 ready 任务时进入 epoll_wait，timeout 由最近定时器决定。
  poller_->wait_events(timeout_ms,
                       [this](const std::shared_ptr<IoWaiter>& waiter, uint32_t ready_events) {
                         handle_io_ready(waiter, ready_events);
                       });
}

void Processor::steal_tasks_when_idle() {
  // 空闲时尝试从其他处理器批量窃取待创建任务，提升整体吞吐。
  const std::vector<std::unique_ptr<Processor>>& all = Runtime::instance().processors();
  if (all.size() <= 1) {
    return;
  }

  auto probe_victim = [&](size_t start_offset) -> Processor* {
    const size_t start = (steal_probe_cursor_ + start_offset) % all.size();
    for (size_t step = 0; step < all.size(); ++step) {
      const size_t index = (start + step) % all.size();
      Processor* candidate = all[index].get();
      if (candidate && candidate != this) {
        return candidate;
      }
    }
    return nullptr;
  };

  Processor* victim_a = probe_victim(0);
  Processor* victim_b = probe_victim(1 + (steal_probe_cursor_ % (all.size() - 1)));
  steal_probe_cursor_ = (steal_probe_cursor_ + 1) % all.size();

  Processor* chosen = victim_a;
  if (victim_a && victim_b && victim_a != victim_b) {
    const uint32_t a_pending = victim_a->pending_task_count();
    const uint32_t b_pending = victim_b->pending_task_count();
    chosen = (b_pending > a_pending) ? victim_b : victim_a;
  }

  std::deque<Task> stolen_batch;
  if (chosen && chosen->steal_tasks(&stolen_batch, 64, 2) > 0) {
  } else if (victim_b && victim_b != chosen && victim_b->steal_tasks(&stolen_batch, 64, 2) > 0) {
  }

  if (stolen_batch.empty()) {
    return;
  }

  ZCOROUTINE_LOG_DEBUG("tasks stolen by scheduler, thief_sched_id={}, stolen={}", id_,
                       stolen_batch.size());
  enqueue_stolen_tasks(&stolen_batch);
}

void Processor::update_load_metrics(uint64_t loop_ns) {
  cpu_time_ns_.fetch_add(loop_ns, std::memory_order_relaxed);

  uint64_t old_ema = ema_loop_ns_.load(std::memory_order_relaxed);
  while (true) {
    const uint64_t new_ema = (old_ema == 0) ? loop_ns : ((old_ema * 7ULL + loop_ns) >> 3);
    if (ema_loop_ns_.compare_exchange_weak(old_ema, new_ema, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
      break;
    }
  }
}

void Processor::wake_loop() {
  if (poller_) {
    poller_->wake();
  }
}

void Processor::drain_new_tasks() {
  std::deque<Task> pending;
  steal_queue_.drain_all(&pending);
  if (pending.empty()) {
    return;
  }

  while (!pending.empty()) {
    Task task = std::move(pending.front());
    pending.pop_front();
    Fiber::ptr fiber = obtain_fiber(std::move(task));
    Runtime::instance().register_fiber(fiber);
    ZCOROUTINE_LOG_DEBUG("task materialized to fiber, sched_id={}, fiber_id={}", id_, fiber->id());
    enqueue_ready(std::move(fiber));
  }
}

void Processor::run_ready_tasks() {
  Fiber::ptr fiber;
  while (dequeue_ready_fiber(&fiber)) {
    if (recycle_if_done_before_run(fiber)) {
      continue;
    }

    Fiber::ptr resumed = switch_to_fiber(std::move(fiber));
    if (!resumed) {
      continue;
    }

    const Fiber::State state = finalize_after_switch(resumed);
    dispatch_resumed_fiber(std::move(resumed), state);
  }
}

bool Processor::dequeue_ready_fiber(Fiber::ptr* fiber) {
  if (!fiber) {
    return false;
  }

  std::lock_guard<std::mutex> lock(run_queue_mutex_);
  if (run_queue_.empty()) {
    return false;
  }

  *fiber = std::move(run_queue_.front());
  run_queue_.pop_front();
  ready_size_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

bool Processor::recycle_if_done_before_run(const Fiber::ptr& fiber) {
  if (!fiber) {
    return true;
  }

  if (fiber->state() != Fiber::State::kDone) {
    return false;
  }

  ZCOROUTINE_LOG_DEBUG("skip done fiber before run, sched_id={}, fiber_id={}", id_, fiber->id());
  Runtime::instance().unregister_fiber(fiber.get());
  recycle_fiber(fiber);
  return true;
}

Fiber::ptr Processor::switch_to_fiber(Fiber::ptr fiber) {
  current_fiber_ = std::move(fiber);

  if (!current_fiber_->context_initialized()) {
    // 首次运行需要初始化 ucontext；后续恢复只做栈快照回填。
    current_fiber_->initialize_context();
  }
  restore_fiber_stack(current_fiber_);

  current_fiber_->mark_running();
  Context* fiber_context = current_fiber_->context();
  ZCOROUTINE_LOG_DEBUG("switch to fiber, sched_id={}, fiber_id={}", id_, current_fiber_->id());
  Context::swap_context(&scheduler_context_, fiber_context);

  Fiber::ptr resumed = current_fiber_;
  current_fiber_.reset();
  return resumed;
}

Fiber::State Processor::finalize_after_switch(const Fiber::ptr& fiber) {
  const Fiber::State state = fiber->state();
  if (state != Fiber::State::kDone) {
    // 非结束态都要把共享栈快照保存，供下次恢复继续执行。
    save_fiber_stack(fiber);
    return state;
  }

  fiber->clear_saved_stack();
  return state;
}

void Processor::dispatch_resumed_fiber(Fiber::ptr fiber, Fiber::State state) {
  if (state == Fiber::State::kReady) {
    // 主动 yield 或被唤醒后进入 ready，重新排队等待下一轮调度。
    enqueue_ready(std::move(fiber));
    return;
  }

  if (state != Fiber::State::kDone) {
    return;
  }

  ZCOROUTINE_LOG_DEBUG("fiber completed and unregistered, sched_id={}, fiber_id={}", id_,
                       fiber->id());
  Runtime::instance().unregister_fiber(fiber.get());
  recycle_fiber(fiber);
}

void Processor::process_timers() { timer_queue_.process_due(); }

int Processor::next_timeout_ms() const { return timer_queue_.next_timeout_ms(); }

void Processor::handle_io_ready(const std::shared_ptr<IoWaiter>& waiter, uint32_t ready_events) {
  (void)ready_events;
  if (!waiter) {
    return;
  }

  if (!waiter->active.exchange(false, std::memory_order_acq_rel)) {
    // 说明已被超时路径或其他路径消费，避免重复恢复。
    return;
  }

  if (waiter->timer) {
    waiter->timer->cancelled.store(true, std::memory_order_release);
  }

  if (Fiber::ptr fiber = waiter->fiber.lock()) {
    ZCOROUTINE_LOG_DEBUG("io ready resume fiber, sched_id={}, fd={}, fiber_id={}, ready_events={}",
                         id_, waiter->fd, fiber->id(), ready_events);
    resume_fiber(fiber, false);
  }
}

void Processor::save_fiber_stack(const Fiber::ptr& fiber) {
  if (!fiber) {
    return;
  }

  const size_t stack_slot = fiber->stack_slot();
  const size_t stack_size = shared_stacks_.size(stack_slot);
  void* stack_data = shared_stacks_.data(stack_slot);
  if (stack_size == 0 || !stack_data) {
    return;
  }

  const uintptr_t stack_bottom = reinterpret_cast<uintptr_t>(stack_data);
  const uintptr_t stack_top = stack_bottom + stack_size;
  const uintptr_t stack_sp = reinterpret_cast<uintptr_t>(fiber->context()->get_stack_pointer());
  if (stack_sp == 0) {
    ZCOROUTINE_LOG_WARN("shared stack save skipped, unsupported architecture");
    return;
  }

  if (stack_sp < stack_bottom || stack_sp > stack_top) {
    ZCOROUTINE_LOG_WARN("shared stack save failed, sp out of range, sched_id={}, fiber_id={}, sp={}",
                        id_, fiber->id(), stack_sp);
    return;
  }

  const size_t used = stack_top - stack_sp;
  fiber->save_stack_data(reinterpret_cast<const char*>(stack_sp), used);
  ZCOROUTINE_LOG_DEBUG("shared stack saved, sched_id={}, fiber_id={}, used_bytes={}", id_,
                       fiber->id(), used);
}

void Processor::restore_fiber_stack(const Fiber::ptr& fiber) {
  if (!fiber || !fiber->has_saved_stack()) {
    return;
  }

  const size_t stack_slot = fiber->stack_slot();
  const size_t stack_size = shared_stacks_.size(stack_slot);
  void* stack_data = shared_stacks_.data(stack_slot);
  if (stack_size == 0 || !stack_data) {
    return;
  }

  const size_t used = fiber->saved_stack_size();
  if (used > stack_size) {
    ZCOROUTINE_LOG_WARN(
        "shared stack restore failed, snapshot too large, sched_id={}, fiber_id={}, used={}, stack={}",
        id_, fiber->id(), used, stack_size);
    return;
  }

  char* dst = reinterpret_cast<char*>(stack_data) + (stack_size - used);
  std::memcpy(dst, fiber->saved_stack_data(), used);
  ZCOROUTINE_LOG_DEBUG("shared stack restored, sched_id={}, fiber_id={}, used_bytes={}", id_,
                       fiber->id(), used);
}

Fiber::ptr Processor::obtain_fiber(Task task) {
  size_t stack_slot = 0;
  if (stack_model_ == StackModel::kShared) {
    const size_t stack_count = shared_stacks_.count() == 0 ? 1 : shared_stacks_.count();
    stack_slot = next_stack_slot_.fetch_add(1, std::memory_order_relaxed) % stack_count;
  }

  const int fiber_id = Runtime::instance().next_fiber_id();

  Fiber::ptr fiber = fiber_pool_.acquire();

  if (fiber) {
    fiber->reset(fiber_id, std::move(task), stack_slot);
    return fiber;
  }

  // Task -> Fiber 的“实体化”发生在调度线程，避免跨线程创建上下文。
  return std::make_shared<Fiber>(fiber_id, this, std::move(task), stack_size_, stack_slot,
                                 stack_model_ == StackModel::kShared);
}

void Processor::recycle_fiber(const Fiber::ptr& fiber) {
  fiber_pool_.recycle(fiber);
}

Processor* current_processor() { return tls_processor; }

void set_current_processor(Processor* processor) { tls_processor = processor; }

}  // namespace zcoroutine
