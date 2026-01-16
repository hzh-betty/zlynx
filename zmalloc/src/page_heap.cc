/**
 * @file page_heap.cc
 * @brief PageHeap 实现
 */

#include "page_heap.h"

#include <algorithm>
#include <new>

namespace zmalloc {

PageHeap &PageHeap::Instance() {
  static PageHeap instance;
  return instance;
}

Span *PageHeap::AllocateSpan(size_t num_pages) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 查找合适大小的空闲 Span
  for (size_t i = num_pages; i <= kMaxPages; ++i) {
    if (!free_lists_[i].IsEmpty()) {
      Span *span = free_lists_[i].PopFront();

      // 如果 Span 太大，分割
      if (span->NumPages() > num_pages) {
        // 创建剩余部分的 Span
        Span *remainder = new (std::nothrow) Span();
        if (remainder == nullptr) {
          // 内存不足，返回整个 Span
          return span;
        }

        remainder->SetPageId(span->PageId() + num_pages);
        remainder->SetNumPages(span->NumPages() - num_pages);

        // 更新原 Span
        span->SetNumPages(num_pages);

        // 更新页号映射
        for (size_t j = 0; j < remainder->NumPages(); ++j) {
          page_map_[remainder->PageId() + j] = remainder;
        }

        // 将剩余部分放回空闲链表
        size_t remain_pages = remainder->NumPages();
        if (remain_pages <= kMaxPages) {
          free_lists_[remain_pages].PushFront(remainder);
        } else {
          free_lists_[kMaxPages].PushFront(remainder);
        }
      }

      return span;
    }
  }

  // 没有合适的空闲 Span，从系统申请
  return AllocateFromSystem(num_pages);
}

Span *PageHeap::AllocateFromSystem(size_t num_pages) {
  // 至少申请 kMinSystemAllocPages 页
  size_t alloc_pages = std::max(num_pages, kMinSystemAllocPages);
  size_t alloc_size = PagesToSize(alloc_pages);

  void *ptr = SystemAlloc(alloc_size);
  if (ptr == nullptr) {
    return nullptr;
  }

  // 创建新的 Span
  Span *span = new (std::nothrow) Span();
  if (span == nullptr) {
    SystemFree(ptr, alloc_size);
    return nullptr;
  }

  size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
  span->SetPageId(page_id);
  span->SetNumPages(alloc_pages);

  // 更新页号映射
  for (size_t i = 0; i < alloc_pages; ++i) {
    page_map_[page_id + i] = span;
  }

  // 如果申请的比需要的多，分割
  if (alloc_pages > num_pages) {
    Span *remainder = new (std::nothrow) Span();
    if (remainder != nullptr) {
      remainder->SetPageId(page_id + num_pages);
      remainder->SetNumPages(alloc_pages - num_pages);
      span->SetNumPages(num_pages);

      for (size_t i = 0; i < remainder->NumPages(); ++i) {
        page_map_[remainder->PageId() + i] = remainder;
      }

      size_t remain_pages = remainder->NumPages();
      if (remain_pages <= kMaxPages) {
        free_lists_[remain_pages].PushFront(remainder);
      } else {
        free_lists_[kMaxPages].PushFront(remainder);
      }
    }
  }

  return span;
}

void PageHeap::DeallocateSpan(Span *span) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t page_id = span->PageId();
  size_t num_pages = span->NumPages();

  // 先清除当前 span 的页映射
  for (size_t i = 0; i < span->NumPages(); ++i) {
    page_map_.erase(span->PageId() + i);
  }

  // 尝试向前合并
  if (page_id > 0) {
    auto prev_it = page_map_.find(page_id - 1);
    if (prev_it != page_map_.end()) {
      Span *prev_span = prev_it->second;
      // 检查是否在空闲链表中（通过检查 prev/next 指针）
      if (prev_span->prev != nullptr && prev_span->SizeClassIndex() == 0) {
        // 从空闲链表移除
        size_t prev_pages = prev_span->NumPages();
        if (prev_pages <= kMaxPages) {
          free_lists_[prev_pages].Erase(prev_span);
        } else {
          free_lists_[kMaxPages].Erase(prev_span);
        }

        // 清除 prev_span 的页映射
        for (size_t i = 0; i < prev_span->NumPages(); ++i) {
          page_map_.erase(prev_span->PageId() + i);
        }

        // 合并
        page_id = prev_span->PageId();
        num_pages += prev_span->NumPages();
        delete prev_span;
      }
    }
  }

  // 尝试向后合并
  size_t next_page_id = page_id + num_pages;
  auto next_it = page_map_.find(next_page_id);
  if (next_it != page_map_.end()) {
    Span *next_span = next_it->second;
    if (next_span->prev != nullptr && next_span->SizeClassIndex() == 0) {
      size_t next_pages = next_span->NumPages();
      if (next_pages <= kMaxPages) {
        free_lists_[next_pages].Erase(next_span);
      } else {
        free_lists_[kMaxPages].Erase(next_span);
      }

      // 清除 next_span 的页映射
      for (size_t i = 0; i < next_span->NumPages(); ++i) {
        page_map_.erase(next_span->PageId() + i);
      }

      num_pages += next_span->NumPages();
      delete next_span;
    }
  }

  // 更新合并后的 Span
  span->SetPageId(page_id);
  span->SetNumPages(num_pages);
  span->SetSizeClass(0);

  // 重新建立页号映射
  for (size_t i = 0; i < num_pages; ++i) {
    page_map_[page_id + i] = span;
  }

  // 放入空闲链表
  if (num_pages <= kMaxPages) {
    free_lists_[num_pages].PushFront(span);
  } else {
    free_lists_[kMaxPages].PushFront(span);
  }
}

Span *PageHeap::GetSpanByPageId(size_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = page_map_.find(page_id);
  if (it != page_map_.end()) {
    return it->second;
  }
  return nullptr;
}

} // namespace zmalloc
