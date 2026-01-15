#include "io/fd_context_table.h"

#include <algorithm>

#include "util/zcoroutine_logger.h"

namespace zcoroutine {

FdContextTable::FdContextTable(size_t initial_capacity) {
  contexts_.resize(initial_capacity);
  ZCOROUTINE_LOG_DEBUG("FdContextTable created with capacity={}",
                       initial_capacity);
}

FdContext::ptr FdContextTable::get(int fd) {
  if (fd < 0) {
    return nullptr;
  }

  RWMutex::ReadLock lock(mutex_);
  if (static_cast<size_t>(fd) < contexts_.size()) {
    return contexts_[fd];
  }
  return nullptr;
}

FdContext::ptr FdContextTable::get_or_create(int fd) {
  if (fd < 0) {
    return nullptr;
  }

  // 快速路径：读锁检查是否已存在
  {
    RWMutex::ReadLock lock(mutex_);
    if (static_cast<size_t>(fd) < contexts_.size() && contexts_[fd]) {
      return contexts_[fd];
    }
  }

  // 慢速路径：写锁创建
  RWMutex::WriteLock lock(mutex_);

  // 双重检查：可能其他线程已经创建
  if (static_cast<size_t>(fd) < contexts_.size() && contexts_[fd]) {
    return contexts_[fd];
  }

  return expand_and_create(fd);
}

size_t FdContextTable::size() const {
  RWMutex::ReadLock lock(mutex_);
  return contexts_.size();
}

FdContext::ptr FdContextTable::expand_and_create(int fd) {
  // 扩容（如果需要）
  if (static_cast<size_t>(fd) >= contexts_.size()) {
    size_t old_size = contexts_.size();
    size_t new_size =
        std::max(static_cast<size_t>(fd + 1), old_size + old_size / 2);
    contexts_.resize(new_size);
    ZCOROUTINE_LOG_DEBUG("FdContextTable expanded from {} to {}", old_size,
                         new_size);
  }

  // 创建新的FdContext
  contexts_[fd] = std::make_shared<FdContext>(fd);
  ZCOROUTINE_LOG_DEBUG("FdContextTable created FdContext for fd={}", fd);

  return contexts_[fd];
}

} // namespace zcoroutine
