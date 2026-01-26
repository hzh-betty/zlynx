/**
 * @file transfer_cache.cc
 * @brief TransferCache 实现
 */

#include "transfer_cache.h"

#include <algorithm>
#include <mutex>

namespace zmalloc {

// C++11 需要 constexpr 静态成员的类外定义
constexpr size_t TransferCacheEntry::kMaxCacheSlots;

size_t TransferCacheEntry::insert_range(void *batch[], size_t count) {
  // 短临界区：搬运指针 + 更新 head/count
  std::lock_guard<SpinLock> lock(mtx_);

  // 计算可插入的数量
  size_t available = kMaxCacheSlots - count_;
  size_t to_insert = std::min(count, available);

  for (size_t i = 0; i < to_insert; ++i) {
    slots_[head_] = batch[i];
    head_ = (head_ + 1) & kMask;
  }
  count_ += to_insert;

  return to_insert;
}

size_t TransferCacheEntry::remove_range(void *batch[], size_t count) {
  std::lock_guard<SpinLock> lock(mtx_);

  // 计算可取出的数量
  size_t to_remove = std::min(count, count_);

  for (size_t i = 0; i < to_remove; ++i) {
    batch[i] = slots_[tail_];
    tail_ = (tail_ + 1) & kMask;
  }
  count_ -= to_remove;

  return to_remove;
}

size_t TransferCacheEntry::size() const {
  std::lock_guard<SpinLock> lock(mtx_);
  return count_;
}

size_t TransferCache::insert_range(size_t index, void *batch[], size_t count) {
  return entries_[index].insert_range(batch, count);
}

size_t TransferCache::remove_range(size_t index, void *batch[], size_t count) {
  return entries_[index].remove_range(batch, count);
}

} // namespace zmalloc
