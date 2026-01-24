#include "shared_stack.h"

#include <cstring>

#include "fiber.h"
#include "stack_allocator.h"
#include "thread_context.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

SharedStackBuffer::SharedStackBuffer(size_t stack_size)
    : stack_size_(stack_size) {
  // 使用StackAllocator分配栈内存
  stack_buffer_ = static_cast<char *>(StackAllocator::allocate(stack_size_));
  if (!stack_buffer_) {
    ZCOROUTINE_LOG_ERROR("SharedStackBuffer allocation failed: size={}",
                         stack_size_);
    return;
  }

  // 栈顶指针（栈从高地址向低地址增长）
  stack_bp_ = stack_buffer_ + stack_size_;

  ZCOROUTINE_LOG_DEBUG(
      "SharedStackBuffer created: buffer={}, size={}, stack_top={}",
      static_cast<void *>(stack_buffer_), stack_size_,
      static_cast<void *>(stack_bp_));
}

SharedStackBuffer::~SharedStackBuffer() {
  if (stack_buffer_) {
    ZCOROUTINE_LOG_DEBUG("SharedStackBuffer destroying: buffer={}",
                         static_cast<void *>(stack_buffer_));
    StackAllocator::deallocate(stack_buffer_, stack_size_);
    stack_buffer_ = nullptr;
    stack_bp_ = nullptr;
  }
}

constexpr size_t SharedStack::kDefaultStackSize;
constexpr int SharedStack::kDefaultStackCount;

SharedStack::SharedStack(int count, size_t stack_size)
    : stack_size_(stack_size), count_(count) {
  if (count <= 0) {
    count_ = kDefaultStackCount;
    ZCOROUTINE_LOG_WARN("SharedStack invalid count {}, using default {}", count,
                        kDefaultStackCount);
  }

  if (stack_size == 0) {
    stack_size_ = kDefaultStackSize;
    ZCOROUTINE_LOG_WARN("SharedStack invalid stack_size 0, using default {}",
                        kDefaultStackSize);
  }

  // 创建栈缓冲区数组
  stack_array_.reserve(count_);
  for (int i = 0; i < count_; ++i) {
    stack_array_.push_back(std::make_unique<SharedStackBuffer>(stack_size_));
  }

  ZCOROUTINE_LOG_INFO("SharedStack created: count={}, stack_size={}", count_,
                      stack_size_);
}

SharedStackBuffer *SharedStack::allocate() {
  if (stack_array_.empty()) {
    ZCOROUTINE_LOG_ERROR("SharedStack::allocate failed: no stack buffers");
    return nullptr;
  }

  ++index_;
  size_t idx = index_ % count_;

  ZCOROUTINE_LOG_DEBUG("SharedStack::allocate: idx={}", idx);

  return stack_array_[idx].get();
}

constexpr size_t SwitchStack::kDefaultSwitchStackSize;

SwitchStack::SwitchStack(size_t stack_size) : stack_size_(stack_size) {
  // 使用StackAllocator分配栈内存
  stack_buffer_ = static_cast<char *>(StackAllocator::allocate(stack_size_));
  if (!stack_buffer_) {
    ZCOROUTINE_LOG_ERROR("SwitchStack allocation failed: size={}", stack_size_);
    return;
  }

  // 栈顶指针（栈从高地址向低地址增长）
  stack_bp_ = stack_buffer_ + stack_size_;

  ZCOROUTINE_LOG_DEBUG("SwitchStack created: buffer={}, size={}, stack_top={}",
                       static_cast<void *>(stack_buffer_), stack_size_,
                       static_cast<void *>(stack_bp_));
}

void SwitchStack::switch_func() {
  while (true) {
    auto curr_ptr = ThreadContext::get_current_fiber();
    auto target_ptr = ThreadContext::get_pending_fiber();
    Context *switch_ctx = ThreadContext::get_switch_context();

    if (!curr_ptr || !target_ptr) {
      ZCOROUTINE_LOG_ERROR("switch_func: invalid curr or target fiber");
      return;
    }

    Fiber *curr = curr_ptr.get();
    Fiber *target = target_ptr.get();

    ZCOROUTINE_LOG_DEBUG("switch_func: curr={}, target={}", curr->name(),
                         target->name());

    // 处理当前协程的栈保存（在 switch stack 上执行，安全）
    if (curr->is_shared_stack()) {
      SharedContext *curr_stack_ctx = curr->get_shared_context();
      if (curr_stack_ctx) {
        // 从 curr 的 context 中获取 rsp（已由 swapcontext 保存）
        void *curr_rsp = curr->context()->get_stack_pointer();

        // 保存当前协程的栈内容
        curr_stack_ctx->save_stack_buffer(curr_rsp);

        ZCOROUTINE_LOG_DEBUG("switch_func: saved curr stack, rsp={}", curr_rsp);
      }
    }

    // 处理目标协程的栈恢复（在 switch stack 上执行，安全）
    if (target->is_shared_stack()) {
      SharedContext *target_stack_ctx = target->get_shared_context();
      if (target_stack_ctx) {
        SharedStackBuffer *buffer = target_stack_ctx->shared_stack_buffer();
        if (buffer) {
          // 获取当前占用此共享栈的协程
          Fiber *occupy_fiber = buffer->occupy_fiber();

          // 如果有其他协程占用且不是目标协程且不是当前协程，保存其栈内容
          // 注意：必须先保存其他协程的栈，再设置occupy_fiber，避免竞态条件
          if (occupy_fiber && occupy_fiber != target && occupy_fiber != curr) {
            SharedContext *occupy_stack_ctx =
                occupy_fiber->get_shared_context();
            if (occupy_stack_ctx) {
              void *occupy_rsp = occupy_fiber->context()->get_stack_pointer();
              occupy_stack_ctx->save_stack_buffer(occupy_rsp);
            }
          }

          // 在保存完其他协程的栈之后，再设置目标协程占用此共享栈
          buffer->set_occupy_fiber(target);

          // 恢复目标协程的栈内容
          if (target_stack_ctx->save_buffer() &&
              target_stack_ctx->save_size() > 0) {
            target_stack_ctx->restore_stack_buffer();
            ZCOROUTINE_LOG_DEBUG("switch_func: restored target stack, size={}",
                                 target_stack_ctx->save_size());
          }
        }
      }
    }

    // 设置当前协程为目标
    ThreadContext::set_current_fiber(target_ptr);

    // 切换到目标协程
    Context::swap_context(switch_ctx, target->context());

    // 当有人切换回 switch_context 时，继续循环处理下一次切换
  }
}

SwitchStack::~SwitchStack() {
  if (stack_buffer_) {
    ZCOROUTINE_LOG_DEBUG("SwitchStack destroying: buffer={}",
                         static_cast<void *>(stack_buffer_));
    StackAllocator::deallocate(stack_buffer_, stack_size_);
    stack_buffer_ = nullptr;
    stack_bp_ = nullptr;
  }
}

SharedContext::~SharedContext() {
  if (save_buffer_) {
    StackAllocator::deallocate(save_buffer_, save_buffer_capacity_);
    save_buffer_ = nullptr;
    save_size_ = 0;
    save_buffer_capacity_ = 0;
  }
}

void SharedContext::init_shared(SharedStackBuffer *buffer) {
  is_shared_stack_ = true;
  shared_stack_buffer_ = buffer;
}

void SharedContext::save_stack_buffer(void *stack_sp) {
  if (!is_shared_stack_ || !shared_stack_buffer_ || !stack_sp) {
    return;
  }

  // 计算需要保存的栈大小
  // stack_top 是栈顶（高地址），stack_sp 是当前栈指针（低地址）
  char *stack_base = shared_stack_buffer_->buffer();
  char *stack_top = shared_stack_buffer_->stack_top();
  char *sp = static_cast<char *>(stack_sp);

  // 边界检查：确保栈指针在有效范围内
  if (sp < stack_base || sp >= stack_top) {
    ZCOROUTINE_LOG_ERROR(
        "SharedContext::save_stack_buffer invalid stack_sp: sp={}, base={}, "
        "top={}",
        static_cast<void *>(sp), static_cast<void *>(stack_base),
        static_cast<void *>(stack_top));
    return;
  }

  size_t len = static_cast<size_t>(stack_top - sp);

  // 优化：复用缓冲区，仅当需要更大空间时才重新分配
  if (len > save_buffer_capacity_) {
    if (save_buffer_) {
      StackAllocator::deallocate(save_buffer_, save_buffer_capacity_);
    }
    // 分配时预留一些空间，减少重复分配
    size_t new_capacity = len + (len >> 2);      // 额外25%空间
    new_capacity = (new_capacity + 15) & ~15ULL; // 16字节对齐
    save_buffer_ = static_cast<char *>(StackAllocator::allocate(new_capacity));
    if (!save_buffer_) {
      ZCOROUTINE_LOG_ERROR(
          "SharedContext::save_stack_buffer allocation failed: size={}",
          new_capacity);
      save_buffer_capacity_ = 0;
      return;
    }
    save_buffer_capacity_ = new_capacity;
  }

  save_size_ = len;
  saved_stack_sp_ = stack_sp; // 记录栈指针位置，用于恢复

  // 使用memcpy复制栈数据
  memcpy(save_buffer_, sp, len);

  ZCOROUTINE_LOG_DEBUG("SharedContext::save_stack_buffer: size={}", len);
}

void SharedContext::restore_stack_buffer() {
  if (!is_shared_stack_ || !save_buffer_ || save_size_ == 0 ||
      !saved_stack_sp_) {
    return;
  }

  // 验证 saved_stack_sp_ 是否在共享栈范围内
  char *stack_base = shared_stack_buffer_->buffer();
  char *stack_top = shared_stack_buffer_->stack_top();
  char *sp = static_cast<char *>(saved_stack_sp_);

  if (sp < stack_base || sp >= stack_top) {
    ZCOROUTINE_LOG_ERROR("SharedContext::restore_stack_buffer invalid "
                         "saved_stack_sp: sp={}, base={}, top={}",
                         static_cast<void *>(sp),
                         static_cast<void *>(stack_base),
                         static_cast<void *>(stack_top));
    return;
  }

  // 恢复栈内容
  memcpy(sp, save_buffer_, save_size_);

  ZCOROUTINE_LOG_DEBUG("SharedContext::restore_stack_buffer: size={}",
                       save_size_);
}

void SharedContext::reset() {
  save_size_ = 0;
  saved_stack_sp_ = nullptr;
}

void SharedContext::clear_occupy(const Fiber *owner) {
  if (shared_stack_buffer_ && shared_stack_buffer_->occupy_fiber() == owner) {
    // 只有当owner是占用者时才清除
    shared_stack_buffer_->set_occupy_fiber(nullptr);
  }
}

} // namespace zcoroutine
