#include "zco/internal/shared_stack_buffer.h"

namespace zco {

// SharedStackBuffer / SharedStackPool 负责共享栈模式下的“栈实体”管理：
// - 每个槽位只保存一块可复用的栈内存，避免协程切换时频繁申请/释放。
// - occupy_fiber_ 仅记录当前占用者，用于调度器在共享栈复用前做归属判断。
// - stack_bp_ 预先保存栈顶指针，切换上下文时可直接作为 ucontext 的栈基准。

SharedStackBuffer::SharedStackBuffer(size_t stack_size)
    : stack_buffer_(stack_size == 0 ? nullptr : new char[stack_size]),
      stack_bp_(stack_buffer_ ? stack_buffer_ + stack_size : nullptr),
      stack_size_(stack_size), occupy_fiber_(nullptr) {}

SharedStackBuffer::~SharedStackBuffer() { delete[] stack_buffer_; }

SharedStackBuffer::SharedStackBuffer(SharedStackBuffer &&other) noexcept
    : stack_buffer_(other.stack_buffer_), stack_bp_(other.stack_bp_),
      stack_size_(other.stack_size_), occupy_fiber_(other.occupy_fiber_) {
    // 资源所有权整体转移，源对象清空后保持可析构但不再持有内存。
    other.stack_buffer_ = nullptr;
    other.stack_bp_ = nullptr;
    other.stack_size_ = 0;
    other.occupy_fiber_ = nullptr;
}

SharedStackBuffer &
SharedStackBuffer::operator=(SharedStackBuffer &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    // 先释放当前持有的栈，再接管新对象，避免共享栈复用时泄漏。
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

char *SharedStackBuffer::data() { return stack_buffer_; }

const char *SharedStackBuffer::data() const { return stack_buffer_; }

char *SharedStackBuffer::stack_bp() { return stack_bp_; }

const char *SharedStackBuffer::stack_bp() const { return stack_bp_; }

size_t SharedStackBuffer::size() const { return stack_size_; }

Fiber *SharedStackBuffer::occupy_fiber() const { return occupy_fiber_; }

void SharedStackBuffer::set_occupy_fiber(Fiber *fiber) {
    // 共享栈不会同时被多个 fiber 持有；这里仅记录最后一个占用者。
    occupy_fiber_ = fiber;
}

SharedStackPool::SharedStackPool(size_t stack_count, size_t stack_size)
    : stacks_() {
    // 预分配固定数量的共享栈槽位，避免运行期扩容打断调度热路径。
    stacks_.reserve(stack_count);
    for (size_t i = 0; i < stack_count; ++i) {
        stacks_.emplace_back(stack_size);
    }
}

void *SharedStackPool::data(size_t stack_slot) {
    if (stack_slot >= stacks_.size()) {
        // 越界直接返回空指针，调用方据此判断共享栈配置是否有效。
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

} // namespace zco