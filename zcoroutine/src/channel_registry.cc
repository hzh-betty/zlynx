#include "channel_registry.h"

#include <algorithm>

#include "zcoroutine_logger.h"

namespace zcoroutine {

ChannelRegistry::ChannelRegistry(size_t initial_capacity) {
  contexts_.resize(initial_capacity);
  ZCOROUTINE_LOG_DEBUG("ChannelRegistry created with capacity={}",
                       initial_capacity);
}

Channel::ptr ChannelRegistry::get(int fd) {
  if (fd < 0) {
    return nullptr;
  }

  RWMutex::ReadLock lock(mutex_);
  if (static_cast<size_t>(fd) < contexts_.size()) {
    return contexts_[fd];
  }
  return nullptr;
}

Channel::ptr ChannelRegistry::get_or_create(int fd) {
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

size_t ChannelRegistry::size() const {
  RWMutex::ReadLock lock(mutex_);
  return contexts_.size();
}

Channel::ptr ChannelRegistry::expand_and_create(int fd) {
  // 扩容（如果需要）
  if (static_cast<size_t>(fd) >= contexts_.size()) {
    size_t old_size = contexts_.size();
    size_t new_size =
        std::max(static_cast<size_t>(fd + 1), old_size + old_size / 2);
    contexts_.resize(new_size);
    ZCOROUTINE_LOG_DEBUG("ChannelRegistry expanded from {} to {}", old_size,
                         new_size);
  }

  // 创建新的FdContext
  contexts_[fd] = std::make_shared<Channel>(fd);
  ZCOROUTINE_LOG_DEBUG("ChannelRegistry created Channel for fd={}", fd);

  return contexts_[fd];
}

} // namespace zcoroutine
