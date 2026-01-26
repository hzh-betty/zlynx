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

  if (to_insert == 0) {
    return 0;
  }

  // head_ 可能在数组中间，拆成两段 memcpy
  const size_t first = std::min(to_insert, kMaxCacheSlots - head_);
  std::memcpy(&slots_[head_], batch, first * sizeof(void *));
  const size_t remain = to_insert - first;
  if (remain > 0) {
    std::memcpy(&slots_[0], batch + first, remain * sizeof(void *));
  }

  head_ = (head_ + to_insert) & kMask;
  count_ += to_insert;

  return to_insert;
}

size_t TransferCacheEntry::remove_range(void *batch[], size_t count) {
  std::lock_guard<SpinLock> lock(mtx_);

  // 计算可取出的数量
  size_t to_remove = std::min(count, count_);

  if (to_remove == 0) {
    return 0;
  }

  const size_t first = std::min(to_remove, kMaxCacheSlots - tail_);
  std::memcpy(batch, &slots_[tail_], first * sizeof(void *));
  const size_t remain = to_remove - first;
  if (remain > 0) {
    std::memcpy(batch + first, &slots_[0], remain * sizeof(void *));
  }

  tail_ = (tail_ + to_remove) & kMask;
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
