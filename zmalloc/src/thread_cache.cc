/**
 * @file thread_cache.cc
 * @brief ThreadCache 实现
 */

#include "thread_cache.h"

#include <algorithm>

#include "central_cache.h"
#include "page_heap.h"
#include "size_class.h"

namespace zmalloc {

void FreeList::Push(void *obj) {
  *reinterpret_cast<void **>(obj) = head_;
  head_ = obj;
  ++size_;
}

void FreeList::PushRange(void *start, void *end, size_t count) {
  *reinterpret_cast<void **>(end) = head_;
  head_ = start;
  size_ += count;
}

void *FreeList::Pop() {
  if (head_ == nullptr) {
    return nullptr;
  }
  void *obj = head_;
  head_ = *reinterpret_cast<void **>(head_);
  --size_;
  return obj;
}

void FreeList::PopRange(void **start, void **end, size_t count) {
  *start = head_;
  void *prev = nullptr;
  void *curr = head_;

  for (size_t i = 0; i < count && curr != nullptr; ++i) {
    prev = curr;
    curr = *reinterpret_cast<void **>(curr);
    --size_;
  }

  *end = prev;
  head_ = curr;

  if (prev != nullptr) {
    *reinterpret_cast<void **>(prev) = nullptr;
  }
}

// thread_local 存储
static thread_local ThreadCache *tls_thread_cache = nullptr;

ThreadCache *ThreadCache::GetInstance() {
  if (tls_thread_cache == nullptr) {
    tls_thread_cache = new ThreadCache();
  }
  return tls_thread_cache;
}

ThreadCache::~ThreadCache() {
  // 归还所有缓存给 CentralCache
  for (size_t i = 0; i < kNumSizeClasses; ++i) {
    if (!free_lists_[i].IsEmpty()) {
      ReleaseToCentralCache(free_lists_[i], i);
    }
  }
}

void *ThreadCache::Allocate(size_t size) {
  if (size == 0) {
    size = 1;
  }

  // 大对象直接从 PageHeap 分配
  if (size > kMaxCacheableSize) {
    size_t num_pages = SizeToPages(size);
    Span *span = PageHeap::Instance().AllocateSpan(num_pages);
    if (span == nullptr) {
      return nullptr;
    }
    return span->StartAddress();
  }

  size_t class_index = GetSizeClass(size);
  FreeList &list = free_lists_[class_index];

  // 快速路径：从本地缓存分配
  if (!list.IsEmpty()) {
    return list.Pop();
  }

  // 慢路径：从 CentralCache 获取
  return FetchFromCentralCache(class_index, size);
}

void ThreadCache::Deallocate(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return;
  }

  // 大对象直接归还 PageHeap
  if (size > kMaxCacheableSize) {
    size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
    Span *span = PageHeap::Instance().GetSpanByPageId(page_id);
    if (span != nullptr) {
      PageHeap::Instance().DeallocateSpan(span);
    }
    return;
  }

  size_t class_index = GetSizeClass(size);
  size_t class_size = GetClassSize(class_index);
  FreeList &list = free_lists_[class_index];

  list.Push(ptr);
  cache_size_ += class_size;

  // 如果缓存过多，归还给 CentralCache
  if (list.Size() > list.MaxSize()) {
    ReleaseToCentralCache(list, class_index);
  }

  // 如果总缓存过大，缩减
  if (cache_size_ > kMaxThreadCacheSize) {
    // 简化处理：归还当前类的一半
    ReleaseToCentralCache(list, class_index);
  }
}

void *ThreadCache::FetchFromCentralCache(size_t class_index, size_t size) {
  // 计算批量获取数量（慢启动策略）
  FreeList &list = free_lists_[class_index];
  size_t batch_count = SizeClass::Instance().GetBatchMoveCount(class_index);
  batch_count = std::min(batch_count, list.MaxSize());
  batch_count = std::max(batch_count, static_cast<size_t>(1));

  size_t actual_count = 0;
  void *start = CentralCache::Instance().FetchRange(class_index, batch_count,
                                                    &actual_count);

  if (actual_count == 0) {
    return nullptr;
  }

  // 取一个返回，其余放入本地缓存
  void *result = start;
  if (actual_count > 1) {
    void *next = *reinterpret_cast<void **>(start);
    // 找到链表尾部
    void *tail = start;
    void *curr = next;
    size_t count = 1;
    while (curr != nullptr && count < actual_count) {
      tail = curr;
      curr = *reinterpret_cast<void **>(curr);
      ++count;
    }
    list.PushRange(next, tail, actual_count - 1);
  }

  // 动态调整 MaxSize
  if (list.MaxSize() < batch_count) {
    list.SetMaxSize(list.MaxSize() + 1);
  }

  size_t class_size = GetClassSize(class_index);
  cache_size_ += class_size * (actual_count - 1);

  return result;
}

void ThreadCache::ReleaseToCentralCache(FreeList &list, size_t class_index) {
  size_t release_count = list.Size() / 2;
  if (release_count < 1 && list.Size() > 0) {
    release_count = 1;
  }
  if (release_count == 0) {
    return;
  }

  void *start = nullptr;
  void *end = nullptr;
  list.PopRange(&start, &end, release_count);

  if (start != nullptr) {
    CentralCache::Instance().ReleaseRange(class_index, start, release_count);
    size_t class_size = GetClassSize(class_index);
    cache_size_ -= class_size * release_count;
  }
}

} // namespace zmalloc
