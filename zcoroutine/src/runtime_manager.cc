#include "zcoroutine/internal/runtime_manager.h"

#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>

#include <chrono>
#include <thread>
#include <utility>

#include "zcoroutine/log.h"
#include "zcoroutine/sched.h"

namespace zcoroutine {

// runtime_manager.cc 负责全局运行时协调：
// - 维护 Processor 线程池生命周期。
// - 负责任务投递与调度器句柄分配。
// - 维护 Fiber 裸句柄到 shared_ptr 的生命周期映射。

namespace {

constexpr size_t kDefaultStackSize = 128 * 1024;
constexpr size_t kDefaultSharedStackNum = 8;
constexpr StackModel kDefaultStackModel = StackModel::kShared;

uint64_t decode_fiber_handle(void* handle) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

void* encode_fiber_handle(uint64_t handle_id) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(handle_id));
}

}  // namespace

Runtime& Runtime::instance() {
  static Runtime runtime;
  return runtime;
}

Runtime::Runtime()
    : started_(false),
      rr_index_(0),
      chooser_seed_(
          static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())),
      fiber_id_gen_(1),
        fiber_handle_id_gen_(1),
      processors_(),
      stack_config_mutex_(),
      stack_num_(kDefaultSharedStackNum),
      stack_size_(kDefaultStackSize),
      stack_model_(kDefaultStackModel),
      scheduler_handle_mutex_(),
      scheduler_handles_(),
      fiber_handle_registry_() {}

bool Runtime::set_stack_num(size_t stack_num) {
  if (stack_num == 0) {
    ZCOROUTINE_LOG_WARN("co_stack_num ignored invalid value 0");
    return false;
  }

  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  if (started_.load(std::memory_order_acquire)) {
    ZCOROUTINE_LOG_WARN("co_stack_num must be called before runtime start");
    return false;
  }
  stack_num_ = stack_num;
  return true;
}

bool Runtime::set_stack_size(size_t stack_size) {
  if (stack_size == 0) {
    ZCOROUTINE_LOG_WARN("co_stack_size ignored invalid value 0");
    return false;
  }

  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  if (started_.load(std::memory_order_acquire)) {
    ZCOROUTINE_LOG_WARN("co_stack_size must be called before runtime start");
    return false;
  }
  stack_size_ = stack_size;
  return true;
}

bool Runtime::set_stack_model(StackModel stack_model) {
  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  if (started_.load(std::memory_order_acquire)) {
    ZCOROUTINE_LOG_WARN("co_stack_model must be called before runtime start");
    return false;
  }
  stack_model_ = stack_model;
  return true;
}

size_t Runtime::stack_num() const {
  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  return stack_num_;
}

size_t Runtime::stack_size() const {
  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  return stack_size_;
}

StackModel Runtime::stack_model() const {
  std::lock_guard<std::mutex> lock(stack_config_mutex_);
  return stack_model_;
}

bool Runtime::ensure_started() {
  // 保持延迟启动语义：首次 submit/main_sched/next_sched 时自动 init。
  if (!started_.load(std::memory_order_acquire)) {
    init(0);
  }

  if (processors_.empty()) {
    ZCOROUTINE_LOG_ERROR("runtime unavailable, processor list is empty");
    return false;
  }

  return true;
}

Scheduler* Runtime::ensure_scheduler_handle(size_t scheduler_index) {
  std::lock_guard<std::mutex> lock(scheduler_handle_mutex_);
  while (scheduler_handles_.size() <= scheduler_index) {
    scheduler_handles_.push_back(std::unique_ptr<Scheduler>(new Scheduler(scheduler_handles_.size())));
  }
  return scheduler_handles_[scheduler_index].get();
}

void Runtime::init(uint32_t scheduler_count) {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    ZCOROUTINE_LOG_DEBUG("runtime init skipped, already started");
    return;
  }

  uint32_t final_count = scheduler_count;
  if (final_count == 0) {
    // 用户未指定并发度时，默认使用硬件并发数。
    final_count = std::thread::hardware_concurrency();
    if (final_count == 0) {
      final_count = 1;
    }
  }

  size_t stack_num = 0;
  size_t stack_size = 0;
  StackModel stack_model = StackModel::kShared;
  {
    std::lock_guard<std::mutex> lock(stack_config_mutex_);
    stack_num = stack_num_;
    stack_size = stack_size_;
    stack_model = stack_model_;
  }

  if (stack_num == 0) {
    stack_num = 1;
  }
  if (stack_size == 0) {
    stack_size = kDefaultStackSize;
  }

  processors_.reserve(final_count);
  for (uint32_t i = 0; i < final_count; ++i) {
    // 每个 Processor 对应一个独立调度线程。
    processors_.push_back(std::unique_ptr<Processor>(
        new Processor(static_cast<int>(i), stack_size, stack_num, stack_model)));
  }

  for (size_t i = 0; i < processors_.size(); ++i) {
    processors_[i]->start();
  }
  ZCOROUTINE_LOG_INFO("runtime initialized, scheduler_count={}", processors_.size());
}

void Runtime::shutdown() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    ZCOROUTINE_LOG_DEBUG("runtime shutdown skipped, not started");
    return;
  }

  for (size_t i = 0; i < processors_.size(); ++i) {
    processors_[i]->stop();
  }
  for (size_t i = 0; i < processors_.size(); ++i) {
    processors_[i]->join();
  }

  fiber_handle_registry_.clear();

  processors_.clear();

  {
    std::lock_guard<std::mutex> lock(scheduler_handle_mutex_);
    scheduler_handles_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(stack_config_mutex_);
    stack_num_ = kDefaultSharedStackNum;
    stack_size_ = kDefaultStackSize;
    stack_model_ = kDefaultStackModel;
  }

  ZCOROUTINE_LOG_INFO("runtime shutdown completed");
}

void Runtime::submit(Task task) {
  if (!task) {
    ZCOROUTINE_LOG_WARN("submit ignored null task");
    return;
  }

  if (!ensure_started()) {
    return;
  }

  const size_t index = pick_processor_index();
  // 轮询基线 + 轻量负载感知，避免单点热点。
  ZCOROUTINE_LOG_DEBUG("runtime submit task, sched_id={}", index);
  processors_[index]->enqueue_task(std::move(task));
}

void Runtime::submit_to(size_t scheduler_index, Task task) {
  if (!task) {
    ZCOROUTINE_LOG_WARN("submit_to ignored null task");
    return;
  }

  if (!ensure_started()) {
    return;
  }

  const size_t index = scheduler_index % processors_.size();
  // 显式投递仍需取模，防止越界访问。
  ZCOROUTINE_LOG_DEBUG("runtime submit_to task, requested_sched_id={}, sched_id={}",
                       scheduler_index, index);
  processors_[index]->enqueue_task(std::move(task));
}

Scheduler* Runtime::main_scheduler() {
  if (!ensure_started()) {
    return nullptr;
  }
  return ensure_scheduler_handle(0);
}

Scheduler* Runtime::next_scheduler() {
  if (!ensure_started()) {
    return nullptr;
  }

  const size_t index = pick_processor_index();
  return ensure_scheduler_handle(index);
}

size_t Runtime::pick_processor_index() {
  const size_t count = processors_.size();
  if (count <= 1) {
    return 0;
  }

  const uint64_t ticket = rr_index_.fetch_add(1, std::memory_order_relaxed);
  const size_t first = static_cast<size_t>(ticket % count);

  // 启动初期先用 RR 快速铺开，避免统计未稳定时偏置。
  if (ticket < static_cast<uint64_t>(count * 2)) {
    return first;
  }

  const size_t second = pick_secondary_index(first, ticket);
  const uint64_t first_score = processors_[first]->load_score();
  const uint64_t second_score = processors_[second]->load_score();
  return (first_score <= second_score) ? first : second;
}

size_t Runtime::pick_secondary_index(size_t first, uint64_t ticket) {
  const size_t count = processors_.size();
  if (count <= 1) {
    return 0;
  }

  uint64_t x = ticket ^ chooser_seed_.fetch_add(0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;

  size_t second = static_cast<size_t>(x % count);
  if (second == first) {
    second = (second + 1) % count;
  }
  return second;
}

void Runtime::resume_external(void* handle) {
  if (!handle) {
    ZCOROUTINE_LOG_WARN("resume_external ignored null fiber");
    return;
  }

  const uint64_t handle_id = decode_fiber_handle(handle);
  // 先通过句柄映射恢复受管对象，避免对象复用导致的 stale handle 误命中。
  std::shared_ptr<Fiber> holder = fiber_handle_registry_.find_by_handle(handle_id);
  if (!holder) {
    ZCOROUTINE_LOG_WARN("resume_external failed, fiber handle not found, handle_id={}", handle_id);
    return;
  }

  if (!holder) {
    ZCOROUTINE_LOG_WARN("resume_external failed, fiber already released");
    return;
  }

  resume_fiber(holder, false);
}

size_t Runtime::scheduler_count() const { return processors_.size(); }

const std::vector<std::unique_ptr<Processor>>& Runtime::processors() const { return processors_; }

int Runtime::next_fiber_id() { return fiber_id_gen_.fetch_add(1, std::memory_order_relaxed); }

void Runtime::register_fiber(const std::shared_ptr<Fiber>& fiber) {
  if (!fiber) {
    return;
  }

  uint64_t existing_handle_id = 0;
  if (fiber_handle_registry_.try_get_handle_id(fiber.get(), &existing_handle_id)) {
    fiber_handle_registry_.register_fiber(fiber, existing_handle_id);
    return;
  }

  const uint64_t handle_id = fiber_handle_id_gen_.fetch_add(1, std::memory_order_relaxed);
  // 记录句柄映射，供 current_coroutine()/resume(void*) 跨 API 使用。
  fiber_handle_registry_.register_fiber(fiber, handle_id);
  ZCOROUTINE_LOG_DEBUG("fiber registered, fiber_id={}, handle_id={}", fiber->id(), handle_id);
}

void Runtime::unregister_fiber(Fiber* fiber) {
  if (!fiber) {
    return;
  }

  uint64_t handle_id = 0;
  if (!fiber_handle_registry_.try_get_handle_id(fiber, &handle_id)) {
    return;
  }

  fiber_handle_registry_.unregister_fiber(fiber);
  ZCOROUTINE_LOG_DEBUG("fiber unregistered, fiber_id={}, handle_id={}", fiber->id(), handle_id);
}

void* Runtime::external_handle(const Fiber* fiber) const {
  if (!fiber) {
    return nullptr;
  }

  uint64_t handle_id = 0;
  if (!fiber_handle_registry_.try_get_handle_id(fiber, &handle_id)) {
    return nullptr;
  }
  return encode_fiber_handle(handle_id);
}

std::shared_ptr<Fiber> current_fiber_shared() {
  Processor* processor = current_processor();
  if (!processor) {
    return nullptr;
  }
  return processor->current_fiber();
}

void resume_fiber(const std::shared_ptr<Fiber>& fiber, bool timed_out) {
  if (!fiber) {
    return;
  }

  if (!fiber->try_wake(timed_out)) {
    ZCOROUTINE_LOG_DEBUG("resume_fiber ignored, fiber not in waiting state, fiber_id={}",
                         fiber->id());
    return;
  }

  Processor* owner = fiber->owner();
  if (!owner) {
    ZCOROUTINE_LOG_WARN("resume_fiber failed, owner processor missing, fiber_id={}", fiber->id());
    return;
  }

  owner->enqueue_ready(fiber);
}

void prepare_current_wait() {
  Processor* processor = current_processor();
  if (!processor) {
    return;
  }
  processor->prepare_wait_current();
}

bool park_current() {
  Processor* processor = current_processor();
  if (!processor) {
    return false;
  }
  return processor->park_current();
}

bool park_current_for(uint32_t milliseconds) {
  Processor* processor = current_processor();
  if (!processor) {
    return false;
  }
  return processor->park_current_for(milliseconds);
}

std::shared_ptr<TimerToken> add_timer(uint32_t milliseconds, std::function<void()> callback) {
  Processor* processor = current_processor();
  if (!processor) {
    return nullptr;
  }
  return processor->add_timer(milliseconds, std::move(callback));
}

bool wait_fd(int fd, uint32_t events, uint32_t milliseconds) {
  Processor* processor = current_processor();
  if (!processor || !processor->current_fiber()) {
    errno = EPERM;
    ZCOROUTINE_LOG_FATAL("wait_fd must be called in coroutine context, fd={}, events={}, timeout_ms={}",
                         fd, events, milliseconds);
    return false;
  }

  // 仅支持协程上下文：统一走 Processor epoll + 挂起模型。
  return processor->wait_fd(fd, events, milliseconds);
}

}  // namespace zcoroutine
