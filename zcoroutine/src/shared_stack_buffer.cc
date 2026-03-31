#include "zcoroutine/internal/shared_stack_buffer.h"

namespace zcoroutine {

SharedStackBuffer::SharedStackBuffer(size_t stack_size)
    : stack_buffer_(stack_size == 0 ? nullptr : new char[stack_size]),
      stack_bp_(stack_buffer_ ? stack_buffer_ + stack_size : nullptr),
      stack_size_(stack_size),
      occupy_fiber_(nullptr) {}

SharedStackBuffer::~SharedStackBuffer() { delete[] stack_buffer_; }

SharedStackBuffer::SharedStackBuffer(SharedStackBuffer&& other) noexcept
    : stack_buffer_(other.stack_buffer_),
      stack_bp_(other.stack_bp_),
      stack_size_(other.stack_size_),
      occupy_fiber_(other.occupy_fiber_) {
  other.stack_buffer_ = nullptr;
  other.stack_bp_ = nullptr;
  other.stack_size_ = 0;
  other.occupy_fiber_ = nullptr;
}

SharedStackBuffer& SharedStackBuffer::operator=(SharedStackBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  delete[] stack_buffer_;
  stack_buffer_ = other.stack_buffer_;
  stack_bp_ = other.stack_bp_;
  stack_size_ = other.stack_size_;
  occupy_fiber_ = other.occupy_fiber_;

  other.stack_buffer_ = nullptr;
  other.stack_bp_ = nullptr;
  other.stack_size_ = 0;
  other.occupy_fiber_ = nullptr;

  return *this;
}

char* SharedStackBuffer::data() { return stack_buffer_; }

const char* SharedStackBuffer::data() const { return stack_buffer_; }

char* SharedStackBuffer::stack_bp() { return stack_bp_; }

const char* SharedStackBuffer::stack_bp() const { return stack_bp_; }

size_t SharedStackBuffer::size() const { return stack_size_; }

Fiber* SharedStackBuffer::occupy_fiber() const { return occupy_fiber_; }

void SharedStackBuffer::set_occupy_fiber(Fiber* fiber) { occupy_fiber_ = fiber; }

SharedStackPool::SharedStackPool(size_t stack_count, size_t stack_size) : stacks_() {
  stacks_.reserve(stack_count);
  for (size_t i = 0; i < stack_count; ++i) {
    stacks_.emplace_back(stack_size);
  }
}

void* SharedStackPool::data(size_t stack_slot) {
  if (stack_slot >= stacks_.size()) {
    return nullptr;
  }
  return stacks_[stack_slot].data();
}

size_t SharedStackPool::size(size_t stack_slot) const {
  if (stack_slot >= stacks_.size()) {
    return 0;
  }
  return stacks_[stack_slot].size();
}

size_t SharedStackPool::count() const { return stacks_.size(); }

}  // namespace zcoroutine