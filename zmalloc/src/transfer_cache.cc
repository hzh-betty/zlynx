/**
 * @file transfer_cache.cc
 * @brief TransferCache 实现
 */

#include "transfer_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace zmalloc {

// C++11 需要 constexpr 静态成员的类外定义
constexpr size_t TransferCacheEntry::kMaxCacheSlots;

size_t TransferCacheEntry::insert_range(void *batch[], size_t count) {
  if (count == 0) {
    return 0;
  }

  // 快路径：锁外快速判断是否已满，尽量减少锁竞争。
  const size_t cur = count_.load(std::memory_order_relaxed);
  if (cur >= kMaxCacheSlots) {
    return 0;
  }

  // 短临界区：搬运指针 + 更新 head/count
  std::lock_guard<SpinLock> lock(mtx_);

  // 计算可插入的数量
  const size_t cur_locked = count_.load(std::memory_order_relaxed);
  const size_t available = kMaxCacheSlots - cur_locked;
  const size_t to_insert = std::min(count, available);

  // 二次确认：锁内重新计算，确保并发下正确。
  if (to_insert == 0) {
    return 0;
  }

  // head_ 可能在数组中间，拆成两段 memcpy
  const size_t first = std::min(to_insert, kMaxCacheSlots - head_);
  std::memcpy(&slots_[head_], batch, first * sizeof(void *));
  if (to_insert > first) {
    std::memcpy(&slots_[0], batch + first,
                (to_insert - first) * sizeof(void *));
  }

  head_ = (head_ + to_insert) & kMask;
  count_.store(cur_locked + to_insert, std::memory_order_relaxed);

  return to_insert;
}

size_t TransferCacheEntry::remove_range(void *batch[], size_t count) {
  if (count == 0) {
    return 0;
  }

  // 快路径：锁外快速判断是否为空，尽量减少锁竞争。
  if (count_.load(std::memory_order_relaxed) == 0) {
    return 0;
  }

  std::lock_guard<SpinLock> lock(mtx_);

  // 计算可取出的数量
  const size_t cur_locked = count_.load(std::memory_order_relaxed);
  const size_t to_remove = std::min(count, cur_locked);

  // 二次确认：锁内重新计算，确保并发下正确。
  if (to_remove == 0) {
    return 0;
  }

  const size_t first = std::min(to_remove, kMaxCacheSlots - tail_);
  std::memcpy(batch, &slots_[tail_], first * sizeof(void *));
  if (to_remove > first) {
    std::memcpy(batch + first, &slots_[0],
                (to_remove - first) * sizeof(void *));
  }

  tail_ = (tail_ + to_remove) & kMask;
  count_.store(cur_locked - to_remove, std::memory_order_relaxed);

  return to_remove;
}

bool TransferCacheEntry::try_insert_range(void *batch[], size_t count,
                                          size_t &inserted) {
  inserted = 0;
  if (count == 0) {
    return true;
  }

  // 快路径：锁外快速判断是否已满
  const size_t cur = count_.load(std::memory_order_relaxed);
  if (cur >= kMaxCacheSlots) {
    return true; // 已满，但操作成功完成（插入0个）
  }

  // 尝试获取锁，失败则立即返回
  if (!mtx_.try_lock()) {
    return false; // 锁竞争，返回 false 让调用方 fallback
  }

  // 计算可插入的数量
  const size_t cur_locked = count_.load(std::memory_order_relaxed);
  const size_t available = kMaxCacheSlots - cur_locked;
  const size_t to_insert = std::min(count, available);

  if (to_insert > 0) {
    const size_t first = std::min(to_insert, kMaxCacheSlots - head_);
    std::memcpy(&slots_[head_], batch, first * sizeof(void *));
    if (to_insert > first) {
      std::memcpy(&slots_[0], batch + first,
                  (to_insert - first) * sizeof(void *));
    }
    head_ = (head_ + to_insert) & kMask;
    count_.store(cur_locked + to_insert, std::memory_order_relaxed);
  }

  mtx_.unlock();
  inserted = to_insert;
  return true;
}

bool TransferCacheEntry::try_remove_range(void *batch[], size_t count,
                                          size_t &removed) {
  removed = 0;
  if (count == 0) {
    return true;
  }

  // 快路径：锁外快速判断是否为空
  if (count_.load(std::memory_order_relaxed) == 0) {
    return true; // 为空，但操作成功完成（获取0个）
  }

  // 尝试获取锁，失败则立即返回
  if (!mtx_.try_lock()) {
    return false; // 锁竞争，返回 false 让调用方 fallback
  }

  // 计算可取出的数量
  const size_t cur_locked = count_.load(std::memory_order_relaxed);
  const size_t to_remove = std::min(count, cur_locked);

  if (to_remove > 0) {
    const size_t first = std::min(to_remove, kMaxCacheSlots - tail_);
    std::memcpy(batch, &slots_[tail_], first * sizeof(void *));
    if (to_remove > first) {
      std::memcpy(batch + first, &slots_[0],
                  (to_remove - first) * sizeof(void *));
    }
    tail_ = (tail_ + to_remove) & kMask;
    count_.store(cur_locked - to_remove, std::memory_order_relaxed);
  }

  mtx_.unlock();
  removed = to_remove;
  return true;
}

size_t TransferCacheEntry::size() const {
  return count_.load(std::memory_order_relaxed);
}

size_t TransferCache::insert_range(size_t index, void *batch[], size_t count) {
  return entries_[index].insert_range(batch, count);
}

size_t TransferCache::remove_range(size_t index, void *batch[], size_t count) {
  return entries_[index].remove_range(batch, count);
}

bool TransferCache::try_insert_range(size_t index, void *batch[], size_t count,
                                     size_t &inserted) {
  return entries_[index].try_insert_range(batch, count, inserted);
}

bool TransferCache::try_remove_range(size_t index, void *batch[], size_t count,
                                     size_t &removed) {
  return entries_[index].try_remove_range(batch, count, removed);
}

} // namespace zmalloc
