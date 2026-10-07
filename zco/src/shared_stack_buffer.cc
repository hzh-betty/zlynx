/**
 * @file shared_stack_buffer.cc
 * @brief shared_stack_buffer 实现。
 * @author hzh-betty
 */

#include "zco/internal/shared_stack_buffer.h"

namespace zco {

// SharedStackBuffer 负责共享栈模式下的“栈实体”管理：
// - 每个槽位只保存一块可复用的栈内存，避免协程切换时频繁申请/释放。
// - occupy_fiber_ 仅记录当前占用者，用于调度器在共享栈复用前做归属判断。

SharedStackBuffer::SharedStackBuffer(size_t stack_size)
    : stack_buffer_(stack_size == 0 ? nullptr : new char[stack_size]),
      stack_size_(stack_size), occupy_fiber_() {}

SharedStackBuffer::~SharedStackBuffer() { delete[] stack_buffer_; }

SharedStackBuffer::SharedStackBuffer(SharedStackBuffer &&other) noexcept
    : stack_buffer_(other.stack_buffer_),
      stack_size_(other.stack_size_), occupy_fiber_(other.occupy_fiber_) {
    // 资源所有权整体转移，源对象清空后保持可析构但不再持有内存。
    other.stack_buffer_ = nullptr;
    other.stack_size_ = 0;
    other.occupy_fiber_ = SharedStackOwner();
}

SharedStackBuffer &
SharedStackBuffer::operator=(SharedStackBuffer &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    // 先释放当前持有的栈，再接管新对象，避免共享栈复用时泄漏。
    delete[] stack_buffer_;
    stack_buffer_ = other.stack_buffer_;
    stack_size_ = other.stack_size_;
    occupy_fiber_ = other.occupy_fiber_;

    other.stack_buffer_ = nullptr;
    other.stack_size_ = 0;
    other.occupy_fiber_ = SharedStackOwner();

    return *this;
}

char *SharedStackBuffer::data() { return stack_buffer_; }

const char *SharedStackBuffer::data() const { return stack_buffer_; }

size_t SharedStackBuffer::size() const { return stack_size_; }

SharedStackOwner SharedStackBuffer::occupy_fiber() const {
    return occupy_fiber_;
}

void SharedStackBuffer::set_occupy_fiber(Fiber *fiber, int fiber_id) {
    // 共享栈不会同时被多个 fiber 持有；这里仅记录最后一个占用者。
    occupy_fiber_ = SharedStackOwner(fiber, fiber ? fiber_id : 0);
}

} // namespace zco
