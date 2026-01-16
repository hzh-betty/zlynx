/**
 * @file page_cache.cc
 * @brief PageCache 实现
 */

#include "page_cache.h"

namespace zmalloc {

Span *PageCache::new_span(size_t k) {
  assert(k > 0);

  // 大于 128 页直接向系统申请
  if (k > NPAGES - 1) {
    void *ptr = system_alloc(k);
    Span *span = span_pool_.allocate();
    span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
    span->n = k;
    span->is_use = true;
    id_span_map_.set(span->page_id, span);
    return span;
  }

  // 先检查第 k 个桶是否有 Span
  if (!span_lists_[k].empty()) {
    Span *k_span = span_lists_[k].pop_front();

    // 建立页号与 Span 的映射
    for (PageId i = 0; i < k_span->n; ++i) {
      id_span_map_.set(k_span->page_id + i, k_span);
    }
    return k_span;
  }

  // 检查后面更大的桶是否有 Span 可以切分
  for (size_t i = k + 1; i < NPAGES; ++i) {
    if (!span_lists_[i].empty()) {
      Span *n_span = span_lists_[i].pop_front();
      Span *k_span = span_pool_.allocate();

      // 在 n_span 头部切 k 页
      k_span->page_id = n_span->page_id;
      k_span->n = k;

      n_span->page_id += k;
      n_span->n -= k;

      // 剩余部分挂到对应桶
      span_lists_[n_span->n].push_front(n_span);

      // 存储 n_span 的首尾页映射，用于后续合并
      id_span_map_.set(n_span->page_id, n_span);
      id_span_map_.set(n_span->page_id + n_span->n - 1, n_span);

      // 建立 k_span 所有页的映射
      for (PageId j = 0; j < k_span->n; ++j) {
        id_span_map_.set(k_span->page_id + j, k_span);
      }
      return k_span;
    }
  }

  // 没有大页 Span，向系统申请 128 页
  Span *big_span = span_pool_.allocate();
  void *ptr = system_alloc(NPAGES - 1);
  big_span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
  big_span->n = NPAGES - 1;

  span_lists_[big_span->n].push_front(big_span);

  // 递归调用
  return new_span(k);
}

Span *PageCache::map_object_to_span(void *obj) {
  PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
  Span *ret = static_cast<Span *>(id_span_map_.get(id));
  assert(ret != nullptr);
  return ret;
}

void PageCache::release_span_to_page_cache(Span *span) {
  // 大于 128 页直接释放给系统
  if (span->n > NPAGES - 1) {
    void *ptr = reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
    system_free(ptr, span->n);
    span_pool_.deallocate(span);
    return;
  }

  // 1. 向前合并
  while (true) {
    PageId prev_id = span->page_id - 1;
    Span *ret = static_cast<Span *>(id_span_map_.get(prev_id));
    if (ret == nullptr) {
      break;
    }
    Span *prev_span = ret;
    if (prev_span->is_use) {
      break;
    }
    if (prev_span->n + span->n > NPAGES - 1) {
      break;
    }

    span->page_id = prev_span->page_id;
    span->n += prev_span->n;

    span_lists_[prev_span->n].erase(prev_span);
    span_pool_.deallocate(prev_span);
  }

  // 2. 向后合并
  while (true) {
    PageId next_id = span->page_id + span->n;
    Span *ret = static_cast<Span *>(id_span_map_.get(next_id));
    if (ret == nullptr) {
      break;
    }
    Span *next_span = ret;
    if (next_span->is_use) {
      break;
    }
    if (next_span->n + span->n > NPAGES - 1) {
      break;
    }

    span->n += next_span->n;

    span_lists_[next_span->n].erase(next_span);
    span_pool_.deallocate(next_span);
  }

  // 挂到对应桶
  span_lists_[span->n].push_front(span);

  // 建立首尾页映射
  id_span_map_.set(span->page_id, span);
  id_span_map_.set(span->page_id + span->n - 1, span);

  span->is_use = false;
}

} // namespace zmalloc
