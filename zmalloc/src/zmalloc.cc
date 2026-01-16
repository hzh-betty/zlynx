/**
 * @file zmalloc.cc
 * @brief zmalloc 主接口实现
 */

#include "zmalloc.h"

#include <cstring>

#include "common.h"
#include "page_heap.h"
#include "size_class.h"
#include "thread_cache.h"

namespace zmalloc {

void *Allocate(size_t size) {
  return ThreadCache::GetInstance()->Allocate(size);
}

void Deallocate(void *ptr) {
  if (ptr == nullptr) {
    return;
  }

  // 需要知道对象大小才能正确归还
  // 从页号查找 Span，获取 SizeClass
  size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
  Span *span = PageHeap::Instance().GetSpanByPageId(page_id);

  if (span == nullptr) {
    return; // 无效指针
  }

  size_t class_index = span->SizeClassIndex();
  if (class_index == 0) {
    // 大对象，直接归还 PageHeap
    PageHeap::Instance().DeallocateSpan(span);
  } else {
    size_t size = SizeClass::Instance().GetClassSize(class_index);
    ThreadCache::GetInstance()->Deallocate(ptr, size);
  }
}

void *Reallocate(void *ptr, size_t new_size) {
  if (ptr == nullptr) {
    return Allocate(new_size);
  }

  if (new_size == 0) {
    Deallocate(ptr);
    return nullptr;
  }

  // 获取原大小
  size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
  Span *span = PageHeap::Instance().GetSpanByPageId(page_id);

  if (span == nullptr) {
    return nullptr;
  }

  size_t old_size;
  size_t class_index = span->SizeClassIndex();
  if (class_index == 0) {
    old_size = span->TotalBytes();
  } else {
    old_size = SizeClass::Instance().GetClassSize(class_index);
  }

  // 如果新大小小于等于原大小，直接返回（简化处理）
  if (new_size <= old_size) {
    return ptr;
  }

  // 分配新内存，复制数据，释放旧内存
  void *new_ptr = Allocate(new_size);
  if (new_ptr == nullptr) {
    return nullptr;
  }

  std::memcpy(new_ptr, ptr, old_size);
  Deallocate(ptr);

  return new_ptr;
}

void *AllocateAligned(size_t size, size_t alignment) {
  // 简化实现：分配更大的块并手动对齐
  if (alignment <= 8) {
    return Allocate(size); // 默认已 8 字节对齐
  }

  // 分配额外空间用于对齐和存储原始指针
  size_t extra = alignment - 1 + sizeof(void *);
  void *raw = Allocate(size + extra);
  if (raw == nullptr) {
    return nullptr;
  }

  // 计算对齐后的地址
  void *aligned = reinterpret_cast<void *>(
      (reinterpret_cast<size_t>(raw) + sizeof(void *) + alignment - 1) &
      ~(alignment - 1));

  // 在对齐地址前存储原始指针
  *(reinterpret_cast<void **>(aligned) - 1) = raw;

  return aligned;
}

void *AllocateZero(size_t num, size_t size) {
  size_t total = num * size;
  void *ptr = Allocate(total);
  if (ptr != nullptr) {
    std::memset(ptr, 0, total);
  }
  return ptr;
}

size_t GetAllocatedSize(void *ptr) {
  if (ptr == nullptr) {
    return 0;
  }

  size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
  Span *span = PageHeap::Instance().GetSpanByPageId(page_id);

  if (span == nullptr) {
    return 0;
  }

  size_t class_index = span->SizeClassIndex();
  if (class_index == 0) {
    return span->TotalBytes();
  }
  return SizeClass::Instance().GetClassSize(class_index);
}

} // namespace zmalloc
