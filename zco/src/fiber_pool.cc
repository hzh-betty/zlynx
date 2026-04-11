#include "zco/internal/fiber_pool.h"

namespace zco {

// FiberPool 保存已完成或可复用的 Fiber，目标是减少热路径上的堆分配：
// - acquire() 走
// FIFO，优先复用更早回收的对象，降低“最近活跃对象”被频繁抖动的概率。
// - recycle() 受 max_size_ 限制，避免池化策略在高峰期无界吃内存。

FiberPool::FiberPool(size_t max_size)
    : max_size_(max_size), mutex_(), fibers_() {}

Fiber::ptr FiberPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fibers_.empty()) {
        return nullptr;
    }

    Fiber::ptr fiber = fibers_.front();
    fibers_.pop_front();
    return fiber;
}

void FiberPool::recycle(const Fiber::ptr &fiber) {
    if (!fiber) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (fibers_.size() >= max_size_) {
        // 池满时直接丢弃，依赖 shared_ptr 自然析构回收底层资源。
        return;
    }
    fibers_.push_back(fiber);
}

void FiberPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    fibers_.clear();
}

size_t FiberPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fibers_.size();
}

} // namespace zco