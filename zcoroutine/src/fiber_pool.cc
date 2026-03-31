#include "zcoroutine/internal/fiber_pool.h"

namespace zcoroutine {

FiberPool::FiberPool(size_t max_size) : max_size_(max_size), mutex_(), fibers_() {}

std::shared_ptr<Fiber> FiberPool::acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fibers_.empty()) {
    return nullptr;
  }

  std::shared_ptr<Fiber> fiber = fibers_.front();
  fibers_.pop_front();
  return fiber;
}

void FiberPool::recycle(const std::shared_ptr<Fiber>& fiber) {
  if (!fiber) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (fibers_.size() >= max_size_) {
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

}  // namespace zcoroutine