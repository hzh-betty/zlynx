/**
 * @file thread_cache.cc
 * @brief ThreadCache 实现
 */

#include "thread_cache.h"
#include "central_cache.h"
#include "transfer_cache.h"

#include <algorithm>

namespace zmalloc {

// TLS 线程本地存储（热点：避免每次分配/释放都付出额外的动态初始化与同步开销）
#if defined(__GNUC__) || defined(__clang__)
static thread_local ThreadCache tls_thread_cache
    __attribute__((tls_model("initial-exec")));
#else
static thread_local ThreadCache tls_thread_cache;
#endif

ThreadCache *get_thread_cache() { return &tls_thread_cache; }

namespace {
constexpr size_t kLargeSizeThreshold = 1024;
constexpr size_t kLargeSizeMinMax = 32;
} // namespace

void *ThreadCache::allocate(size_t size) {
  assert(size <= MAX_BYTES);

  const SizeClassLookup &e = SizeClass::lookup(size);
  const size_t align_size = static_cast<size_t>(e.align_size);
  const size_t index = static_cast<size_t>(e.index);
  const size_t num_move = static_cast<size_t>(e.num_move);

  if (align_size >= kLargeSizeThreshold &&
      free_lists_[index].max_size() < kLargeSizeMinMax) {
    free_lists_[index].max_size() = kLargeSizeMinMax;
  }

  if (ZM_LIKELY(!free_lists_[index].empty())) {
    return free_lists_[index].pop();
  } else {
    // miss：走慢路径，从 TransferCache/CentralCache 批量获取。
    return fetch_from_central_cache(index, align_size, num_move);
  }
}

void ThreadCache::deallocate(void *ptr, size_t size) {
  assert(ptr);
  assert(size <= MAX_BYTES);

  const SizeClassLookup &e = SizeClass::lookup(size);
  const size_t align_size = static_cast<size_t>(e.align_size);
  const size_t index = static_cast<size_t>(e.index);

  if (align_size >= kLargeSizeThreshold &&
      free_lists_[index].max_size() < kLargeSizeMinMax) {
    free_lists_[index].max_size() = kLargeSizeMinMax;
  }
  free_lists_[index].push(ptr);

  // 自由链表过长时，回收到 CentralCache
  if (ZM_UNLIKELY(free_lists_[index].size() >= free_lists_[index].max_size())) {
    list_too_long(free_lists_[index], size, index);
  }
}

void *ThreadCache::fetch_from_central_cache(size_t index, size_t size) {
  // 兼容包装：保留旧签名，内部用 SizeClass 策略计算一次批量参数。
  return fetch_from_central_cache(index, size, SizeClass::num_move_size(size));
}

void *ThreadCache::fetch_from_central_cache(size_t index, size_t size,
                                            size_t num_move) {
  // 慢启动反馈调节：
  // - 批量大小 batch_num 不超过当前 freelist 的 max_size()
  // - 若频繁 miss，会逐步增大 max_size()，减少后续锁争用
  size_t batch_num = std::min(free_lists_[index].max_size(), num_move);
  if (batch_num == free_lists_[index].max_size()) {
    free_lists_[index].max_size() += 1;
  }

  void *batch[128];
  size_t got = 0;

  // 优先尝试从 TransferCache 获取（无阻塞）
  // - try_remove_range 返回 true: 操作完成（got 可能为 0）
  // - try_remove_range 返回 false: 锁竞争，直接 fallback 到 CentralCache
  bool try_success = TransferCache::get_instance().try_remove_range(
      index, batch, batch_num, got);

  if (try_success && got > 0) {
    // TransferCache 命中
    if (got == 1) {
      return batch[0];
    }
    // 将多余对象放入自由链表
    for (size_t i = 1; i < got; ++i) {
      free_lists_[index].push(batch[i]);
    }
    return batch[0];
  }

  // TransferCache 未命中或锁竞争，向 CentralCache 请求
  void *start = nullptr;
  void *end = nullptr;
  size_t actual_num = CentralCache::get_instance().fetch_range_obj(
      start, end, batch_num, size, index);
  assert(actual_num >= 1);

  if (actual_num == 1) {
    assert(start == end);
    return start;
  } else {
    free_lists_[index].push_range(next_obj(start), end, actual_num - 1);
    return start;
  }
}

void ThreadCache::list_too_long(FreeList &list, size_t size, size_t index) {
  // 超过阈值时释放一部分而不是全部。
  // 这样能在高并发下减少“抖动”（刚释放完又立刻去 Central/Transfer 拉取）。
  size_t count = list.max_size() / 2;
  if (count == 0) {
    count = 1;
  }

  if (count > 128) {
    count = 128;
  }
  if (count > list.size()) {
    count = list.size();
  }
  if (count == 0) {
    return;
  }

  // 直接批量 pop 到数组，避免 pop_range + 二次遍历
  void *batch[128];
  const size_t collected = list.pop_batch(batch, count);
  if (collected == 0) {
    return;
  }

  // 尝试插入 TransferCache（无阻塞）
  // - try_insert_range 返回 true: 操作完成（inserted 可能 < collected）
  // - try_insert_range 返回 false: 锁竞争，全部放入 CentralCache
  size_t inserted = 0;
  bool try_success = TransferCache::get_instance().try_insert_range(
      index, batch, collected, inserted);

  size_t remaining_start = try_success ? inserted : 0;

  // 剩余放回 CentralCache
  if (remaining_start < collected) {
    // 为剩余部分重建链表（CentralCache 接口接收链表起点）
    void *start = batch[remaining_start];
    for (size_t i = remaining_start; i + 1 < collected; ++i) {
      next_obj(batch[i]) = batch[i + 1];
    }
    next_obj(batch[collected - 1]) = nullptr;
    CentralCache::get_instance().release_list_to_spans(start, size, index);
  }
}

} // namespace zmalloc
