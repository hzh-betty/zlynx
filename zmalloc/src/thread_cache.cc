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
}

void *ThreadCache::allocate(size_t size) {
  assert(size <= MAX_BYTES);

  size_t align_size = 0;
  size_t index = 0;
  SizeClass::classify(size, align_size, index);

  if (align_size >= kLargeSizeThreshold && free_lists_[index].max_size() < kLargeSizeMinMax) {
    free_lists_[index].max_size() = kLargeSizeMinMax;
  }

  if (ZM_LIKELY(!free_lists_[index].empty())) {
    return free_lists_[index].pop();
  } else {
    return fetch_from_central_cache(index, align_size);
  }
}

void ThreadCache::deallocate(void *ptr, size_t size) {
  assert(ptr);
  assert(size <= MAX_BYTES);

  const size_t align_size = SizeClass::round_up_fast(size);
  size_t index = SizeClass::index_fast(size);

  if (align_size >= kLargeSizeThreshold && free_lists_[index].max_size() < kLargeSizeMinMax) {
    free_lists_[index].max_size() = kLargeSizeMinMax;
  }
  free_lists_[index].push(ptr);

  // 自由链表过长时，回收到 CentralCache
  if (ZM_UNLIKELY(free_lists_[index].size() >= free_lists_[index].max_size())) {
    list_too_long(free_lists_[index], size);
  }
}

void *ThreadCache::fetch_from_central_cache(size_t index, size_t size) {
  // 慢启动反馈调节算法
  size_t batch_num =
      std::min(free_lists_[index].max_size(), SizeClass::num_move_size(size));
  if (batch_num == free_lists_[index].max_size()) {
    free_lists_[index].max_size() += 1;
  }

  // 优先从 TransferCache 获取
  void *batch[128];
  size_t got =
      TransferCache::get_instance().remove_range(index, batch, batch_num);

  if (got > 0) {
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

  // TransferCache 未命中，向 CentralCache 请求
  void *start = nullptr;
  void *end = nullptr;
  size_t actual_num =
      CentralCache::get_instance().fetch_range_obj(start, end, batch_num, size,
                             index);
  assert(actual_num >= 1);

  if (actual_num == 1) {
    assert(start == end);
    return start;
  } else {
    free_lists_[index].push_range(next_obj(start), end, actual_num - 1);
    return start;
  }
}

void ThreadCache::list_too_long(FreeList &list, size_t size) {
  size_t index = SizeClass::index_fast(size);
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

  // 一次性从 ThreadCache 的自由链表摘下一段链表
  void *chain_start = nullptr;
  void *chain_end = nullptr;
  list.pop_range(chain_start, chain_end, count);

  // 收集待回收的对象指针数组（TransferCache 接口需要数组）
  void *batch[128];
  void *cur = chain_start;
  for (size_t i = 0; i < count; ++i) {
    batch[i] = cur;
    cur = next_obj(cur);
  }
  const size_t collected = count;

  // 优先插入 TransferCache
  size_t inserted =
      TransferCache::get_instance().insert_range(index, batch, collected);

  // TransferCache 满了，剩余放回 CentralCache
  if (inserted < collected) {
    if (inserted > 0) {
      // 断开链表：防止把已进入 TransferCache 的部分也回收到 CentralCache
      next_obj(batch[inserted - 1]) = nullptr;
    }
    void *start = batch[inserted];
    CentralCache::get_instance().release_list_to_spans(start, size, index);
  }
}

} // namespace zmalloc
