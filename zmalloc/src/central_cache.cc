/**
 * @file central_cache.cc
 * @brief CentralCache 实现
 */

#include "central_cache.h"

#include "page_heap.h"
#include "size_class.h"

namespace zmalloc {

CentralCache &CentralCache::Instance() {
  static CentralCache instance;
  return instance;
}

void *CentralCache::FetchRange(size_t class_index, size_t batch_count,
                               size_t *actual_count) {
  std::lock_guard<std::mutex> lock(mutexes_[class_index]);

  Span *span = span_lists_[class_index].Front();

  // 如果没有可用的 Span 或当前 Span 没有空闲对象，获取新的
  if (span == nullptr || !span->HasFreeObject()) {
    span = GetSpanFromPageHeap(class_index);
    if (span == nullptr) {
      *actual_count = 0;
      return nullptr;
    }
    span_lists_[class_index].PushFront(span);
  }

  // 从 Span 获取对象
  void *start = nullptr;
  void *end = nullptr;
  size_t count = 0;

  while (count < batch_count && span->HasFreeObject()) {
    void *obj = span->PopFreeObject();
    span->IncrementUseCount();

    *reinterpret_cast<void **>(obj) = start;
    if (start == nullptr) {
      end = obj;
    }
    start = obj;
    ++count;
  }

  *actual_count = count;
  return start;
}

void CentralCache::ReleaseRange(size_t class_index, void *start, size_t count) {
  std::lock_guard<std::mutex> lock(mutexes_[class_index]);

  while (start != nullptr && count > 0) {
    void *next = *reinterpret_cast<void **>(start);

    // 找到对象所属的 Span
    size_t page_id = reinterpret_cast<size_t>(start) >> kPageShift;
    Span *span = PageHeap::Instance().GetSpanByPageId(page_id);

    if (span != nullptr) {
      span->PushFreeObject(start);
      span->DecrementUseCount();

      // 如果 Span 完全空闲，归还给 PageHeap
      if (span->IsFullyFree()) {
        span_lists_[class_index].Erase(span);
        span->SetSizeClass(0);
        PageHeap::Instance().DeallocateSpan(span);
      }
    }

    start = next;
    --count;
  }
}

Span *CentralCache::GetSpanFromPageHeap(size_t class_index) {
  size_t span_pages = SizeClass::Instance().GetSpanPages(class_index);
  size_t class_size = SizeClass::Instance().GetClassSize(class_index);

  Span *span = PageHeap::Instance().AllocateSpan(span_pages);
  if (span == nullptr) {
    return nullptr;
  }

  span->SetSizeClass(class_index);

  // 将 Span 切分为对象并链接
  char *start = static_cast<char *>(span->StartAddress());
  size_t total_bytes = span->TotalBytes();
  size_t num_objects = total_bytes / class_size;

  for (size_t i = 0; i < num_objects; ++i) {
    void *obj = start + i * class_size;
    span->PushFreeObject(obj);
  }

  return span;
}

} // namespace zmalloc
