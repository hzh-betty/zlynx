#include "zcoroutine/internal/fiber.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#include "zcoroutine/internal/processor.h"
#include "zcoroutine/log.h"

namespace zcoroutine {

// fiber.cc 聚焦“协程执行单元”本身：
// - 保存任务函数与运行状态。
// - 管理 ucontext 初始化。
// - 在共享栈模式下保存/恢复栈快照。

namespace {

extern "C" void zcoroutine_context_entry();
constexpr uint8_t kDynamicSnapshotBucketLocal = 0xff;

void zcoroutine_context_entry() {
  // 所有 fiber 首次切入都会经过该入口，再桥接到 Fiber::run。
  Processor* processor = current_processor();
  if (!processor) {
    ZCOROUTINE_LOG_ERROR("context entry has no current processor");
    return;
  }

  std::shared_ptr<Fiber> holder = processor->current_fiber();
  if (!holder) {
    ZCOROUTINE_LOG_ERROR("context entry has no current fiber, sched_id={}", processor->id());
    return;
  }

  holder->run();
}

}  // namespace

Fiber::Fiber(int id,
             Processor* owner,
             Task task,
             size_t stack_size,
             size_t stack_slot,
             bool use_shared_stack)
    : id_(id),
      owner_(owner),
      task_(std::move(task)),
      stack_slot_(stack_slot),
      use_shared_stack_(use_shared_stack),
      independent_stack_buffer_(nullptr),
      independent_stack_size_(0),
      context_(),
      saved_stack_buffer_(nullptr),
      saved_stack_size_(0),
      saved_stack_capacity_(0),
      saved_stack_bucket_(kDynamicSnapshotBucketLocal),
      context_initialized_(false),
      state_(State::kReady),
      timed_out_(false),
      external_handle_id_(0) {
  if (!owner_) {
    throw std::runtime_error("fiber owner is null");
  }

  if (stack_size == 0) {
    throw std::runtime_error("fiber stack_size is invalid");
  }

  if (use_shared_stack_) {
    if (owner_->shared_stack_count() == 0 || stack_slot_ >= owner_->shared_stack_count() ||
        owner_->shared_stack_size(stack_slot_) == 0 || owner_->shared_stack_data(stack_slot_) == nullptr) {
      throw std::runtime_error("shared stack is not initialized");
    }

    ZCOROUTINE_LOG_DEBUG("fiber created(shared stack lazy init), fiber_id={}, sched_id={}, slot={}, "
                         "stack_size={}",
                         id_, owner_ ? owner_->id() : -1, stack_slot_,
                         owner_->shared_stack_size(stack_slot_));
    return;
  }

  independent_stack_buffer_ = new char[stack_size];
  independent_stack_size_ = stack_size;
  ZCOROUTINE_LOG_DEBUG("fiber created(independent stack lazy init), fiber_id={}, sched_id={}, "
                       "stack_size={}",
                       id_, owner_ ? owner_->id() : -1, independent_stack_size_);
}

Fiber::~Fiber() {
  clear_saved_stack();
  delete[] independent_stack_buffer_;
}

int Fiber::id() const { return id_; }

void Fiber::reset(int id, Task task, size_t stack_slot) {
  id_ = id;
  task_ = std::move(task);
  stack_slot_ = stack_slot;
  context_ = Context();
  context_initialized_ = false;
  state_.store(State::kReady, std::memory_order_release);
  timed_out_.store(false, std::memory_order_release);
  external_handle_id_.store(0, std::memory_order_release);
  clear_saved_stack();
}

Processor* Fiber::owner() const { return owner_; }

size_t Fiber::stack_slot() const { return stack_slot_; }

bool Fiber::use_shared_stack() const { return use_shared_stack_; }

uint64_t Fiber::external_handle_id() const {
  return external_handle_id_.load(std::memory_order_acquire);
}

bool Fiber::try_set_external_handle_id(uint64_t handle_id, uint64_t* effective_handle) {
  if (handle_id == 0 || !effective_handle) {
    return false;
  }

  uint64_t expected = 0;
  if (external_handle_id_.compare_exchange_strong(expected, handle_id,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
    *effective_handle = handle_id;
    return true;
  }

  *effective_handle = expected;
  return expected != 0;
}

uint64_t Fiber::clear_external_handle_id() {
  return external_handle_id_.exchange(0, std::memory_order_acq_rel);
}

Context* Fiber::context() { return &context_; }

Fiber::State Fiber::state() const { return state_.load(std::memory_order_acquire); }

void Fiber::mark_running() { state_.store(State::kRunning, std::memory_order_release); }

void Fiber::mark_ready() { state_.store(State::kReady, std::memory_order_release); }

void Fiber::mark_waiting() {
  timed_out_.store(false, std::memory_order_release);
  state_.store(State::kWaiting, std::memory_order_release);
}

void Fiber::mark_done() { state_.store(State::kDone, std::memory_order_release); }

bool Fiber::try_wake(bool timed_out) {
  State expected = State::kWaiting;
  if (state_.compare_exchange_strong(expected, State::kReady, std::memory_order_acq_rel)) {
    timed_out_.store(timed_out, std::memory_order_release);
    return true;
  }
  return false;
}

bool Fiber::timed_out() const { return timed_out_.load(std::memory_order_acquire); }

void Fiber::clear_timed_out() { timed_out_.store(false, std::memory_order_release); }

void Fiber::run() {
  // 任务函数异常不能越过协程边界，否则会破坏调度循环稳定性。
  try {
    if (task_) {
      task_();
    }
  } catch (...) {
    ZCOROUTINE_LOG_ERROR("unhandled exception escaped from fiber, fiber_id={}", id_);
  }

  mark_done();
  ZCOROUTINE_LOG_DEBUG("fiber finished, fiber_id={}", id_);
}

bool Fiber::context_initialized() const { return context_initialized_; }

void Fiber::initialize_context() {
  if (context_initialized_) {
    return;
  }

  if (use_shared_stack_) {
    // Fiber 使用 Processor 的共享栈，uc_link 指向调度上下文保证自然返回可回收。
    // 这意味着 Fiber 执行结束后不需要手动 longjmp，函数 return 即可回到调度器。
    context_.make_context(owner_->shared_stack_data(stack_slot_), owner_->shared_stack_size(stack_slot_),
                          reinterpret_cast<void (*)()>(&zcoroutine_context_entry),
                          owner_->scheduler_context());
  } else {
    context_.make_context(independent_stack_buffer_, independent_stack_size_,
                          reinterpret_cast<void (*)()>(&zcoroutine_context_entry),
                          owner_->scheduler_context());
  }

  context_initialized_ = true;
  ZCOROUTINE_LOG_DEBUG("fiber context initialized, fiber_id={}, sched_id={}, slot={}, model={}", id_,
                       owner_ ? owner_->id() : -1, stack_slot_, use_shared_stack_ ? "shared" : "independent");
}

void Fiber::save_stack_data(const char* data, size_t size) {
  if (!use_shared_stack_) {
    return;
  }

  if (size == 0) {
    clear_saved_stack();
    return;
  }

  if (!owner_) {
    return;
  }

  if (!saved_stack_buffer_ || saved_stack_capacity_ < size) {
    clear_saved_stack();
    saved_stack_buffer_ = owner_->acquire_snapshot_buffer(size, &saved_stack_capacity_,
                                                           &saved_stack_bucket_);
    if (!saved_stack_buffer_ || saved_stack_capacity_ < size) {
      saved_stack_buffer_ = nullptr;
      saved_stack_size_ = 0;
      saved_stack_capacity_ = 0;
      saved_stack_bucket_ = kDynamicSnapshotBucketLocal;
      return;
    }
  }

  // 每次切出时仅保存“已使用”的栈区段，避免复制整块共享栈。
  memcpy(saved_stack_buffer_, data, size);
  saved_stack_size_ = size;
}

bool Fiber::has_saved_stack() const { return saved_stack_buffer_ != nullptr && saved_stack_size_ != 0; }

size_t Fiber::saved_stack_size() const { return saved_stack_size_; }

const char* Fiber::saved_stack_data() const {
  return has_saved_stack() ? saved_stack_buffer_ : nullptr;
}

void Fiber::clear_saved_stack() {
  if (saved_stack_buffer_ && owner_) {
    owner_->release_snapshot_buffer(saved_stack_buffer_, saved_stack_bucket_, saved_stack_capacity_);
  }

  saved_stack_buffer_ = nullptr;
  saved_stack_size_ = 0;
  saved_stack_capacity_ = 0;
  saved_stack_bucket_ = kDynamicSnapshotBucketLocal;
}

}  // namespace zcoroutine
