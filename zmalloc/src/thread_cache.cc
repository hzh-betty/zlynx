/**
 * @file thread_cache.cc
 * @brief ThreadCache 实现
 */

#include "thread_cache.h"
#include "central_cache.h"
#include "object_pool.h"
#include "transfer_cache.h"

#include <algorithm>

namespace zmalloc {

// TLS 线程本地存储
static thread_local ThreadCache *tls_thread_cache = nullptr;

ThreadCache *get_thread_cache() {
  if (tls_thread_cache == nullptr) {
    static std::mutex tc_mtx;
    static ObjectPool<ThreadCache> tc_pool;

    std::lock_guard<std::mutex> lock(tc_mtx);
    tls_thread_cache = tc_pool.allocate();
  }
  return tls_thread_cache;
}

void *ThreadCache::allocate(size_t size) {
  assert(size <= MAX_BYTES);

  size_t align_size = SizeClass::round_up(size);
  size_t index = SizeClass::index(size);

  if (!free_lists_[index].empty()) {
    return free_lists_[index].pop();
  } else {
    return fetch_from_central_cache(index, align_size);
  }
}

void ThreadCache::deallocate(void *ptr, size_t size) {
  assert(ptr);
  assert(size <= MAX_BYTES);

  size_t index = SizeClass::index(size);
  free_lists_[index].push(ptr);

  // 自由链表过长时，回收到 CentralCache
  if (free_lists_[index].size() >= free_lists_[index].max_size()) {
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
      CentralCache::get_instance().fetch_range_obj(start, end, batch_num, size);
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
  size_t index = SizeClass::index(size);
  size_t count = list.max_size();

  // 收集待回收的对象
  void *batch[128];
  size_t collected = 0;
  for (size_t i = 0; i < count && !list.empty() && collected < 128; ++i) {
    batch[collected++] = list.pop();
  }

  // 优先插入 TransferCache
  size_t inserted =
      TransferCache::get_instance().insert_range(index, batch, collected);

  // TransferCache 满了，剩余放回 CentralCache
  if (inserted < collected) {
    void *start = batch[inserted];
    void *end = batch[inserted];
    for (size_t i = inserted + 1; i < collected; ++i) {
      next_obj(end) = batch[i];
      end = batch[i];
    }
    next_obj(end) = nullptr;
    CentralCache::get_instance().release_list_to_spans(start, size);
  }
}

} // namespace zmalloc
