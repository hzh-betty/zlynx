/**
 * @file central_cache.cc
 * @brief CentralCache 实现
 */
#include "central_cache.h"
#include "page_cache.h"

namespace zmalloc {

size_t CentralCache::fetch_range_obj(void *&start, void *&end, size_t n,
                                     size_t size) {
  size_t index = SizeClass::index_fast(size);
  CentralFreeList &free_list = free_lists_[index];
  free_list.lock.lock();

  // 获取一个非空的 Span
  Span *span = get_one_span(free_list, size);
  assert(span);
  assert(span->free_list);

  // 从 Span 获取 n 个对象，不够则有多少拿多少
  start = span->free_list;
  end = start;
  size_t actual_num = 1;
  while (actual_num < n) {
    void *next = next_obj(end);
    if (next == nullptr) {
      break;
    }
    end = next;
    ++actual_num;
  }
  span->free_list = next_obj(end);
  next_obj(end) = nullptr;
  span->use_count += actual_num;

  // 结构化维护：non-empty/empty 双链表（更接近 tcmalloc central freelist）
  if (ZM_UNLIKELY(span->free_list == nullptr)) {
    free_list.nonempty.erase(span);
    free_list.empty.push_front(span);
  }

  free_list.lock.unlock();
  return actual_num;
}

Span *CentralCache::get_one_span(CentralFreeList &free_list, size_t size) {
  // 1. 热点路径：nonempty 链表首部一定是可用 span（O(1)）。
  Span *front = free_list.nonempty.begin();
  if (front != free_list.nonempty.end()) {
    assert(front->free_list != nullptr);
    return front;
  }

  // 2. 没有非空 Span，向 PageCache 申请（先解桶锁，让其他线程释放不阻塞）
  free_list.lock.unlock();

  PageCache::get_instance().page_mtx().lock();
  Span *span =
      PageCache::get_instance().new_span(SizeClass::num_move_page(size));
  span->is_use = true;
  span->obj_size = size;
  PageCache::get_instance().page_mtx().unlock();

  // 计算大块内存的起始地址和字节数
  char *start = reinterpret_cast<char *>(span->page_id << PAGE_SHIFT);
  size_t bytes = span->n << PAGE_SHIFT;

  // 切分成 size 大小的对象
  char *obj_end = start + bytes;
  span->free_list = start;
  start += size;
  void *tail = span->free_list;

  while (start < obj_end) {
    next_obj(tail) = start;
    tail = start;
    start += size;
  }
  next_obj(tail) = nullptr;

  // 重新加桶锁，挂到 nonempty
  free_list.lock.lock();
  free_list.nonempty.push_front(span);

  return span;
}

void CentralCache::release_list_to_spans(void *start, size_t size) {
  size_t index = SizeClass::index_fast(size);
  CentralFreeList &free_list = free_lists_[index];
  if (start == nullptr) {
    return;
  }

  // 两阶段批处理
  // 1) 无锁阶段：把对象按 Span 分组 + 本地拼接，减少 PageMap::get 次数。
  // 2) 持锁阶段：对每个 Span 一次性 splice 链表并批量更新 use_count，缩短桶锁持有时间。
  //
  // 注意：ThreadCache 过长回收最多 128 个对象，因此这里固定上限 128。
  Span *spans[128];
  void *group_start[128];
  void *group_end[128];
  size_t group_count[128];
  size_t groups = 0;

  Span *last_span = nullptr;
  PageId last_begin = 0;
  PageId last_end = 0;

  while (start) {
    void *next = next_obj(start);
    next_obj(start) = nullptr;

    Span *span = nullptr;
    const PageId id = reinterpret_cast<PageId>(start) >> PAGE_SHIFT;
    if (last_span != nullptr && id >= last_begin && id < last_end) {
      span = last_span;
    } else {
      span = PageCache::get_instance().map_object_to_span(start);
      last_span = span;
      last_begin = span->page_id;
      last_end = span->page_id + span->n;
    }

    size_t gi = 0;
    for (; gi < groups; ++gi) {
      if (spans[gi] == span) {
        break;
      }
    }
    if (gi == groups) {
      // new group
      spans[groups] = span;
      group_start[groups] = start;
      group_end[groups] = start;
      group_count[groups] = 1;
      ++groups;
    } else {
      next_obj(group_end[gi]) = start;
      group_end[gi] = start;
      ++group_count[gi];
    }

    start = next;
  }

  // 持锁阶段：每个 Span 一次性 splice + 批量更新。
  free_list.lock.lock();
  for (size_t gi = 0; gi < groups; ++gi) {
    Span *span = spans[gi];
    const bool was_empty = (span->free_list == nullptr);

    // splice: [group_start..group_end] + old span->free_list
    next_obj(group_end[gi]) = span->free_list;
    span->free_list = group_start[gi];

    span->use_count -= group_count[gi];

    if (span->use_count == 0) {
      // Span 全部对象回收，归还 PageCache
      if (was_empty) {
        free_list.empty.erase(span);
      } else {
        free_list.nonempty.erase(span);
      }
      span->free_list = nullptr;
      span->next = nullptr;
      span->prev = nullptr;

      free_list.lock.unlock();
      PageCache::get_instance().page_mtx().lock();
      PageCache::get_instance().release_span_to_page_cache(span);
      PageCache::get_instance().page_mtx().unlock();
      free_list.lock.lock();
      continue;
    }

    if (was_empty) {
      // empty -> nonempty
      free_list.empty.erase(span);
      free_list.nonempty.push_front(span);
    }
  }
  free_list.lock.unlock();
}

} // namespace zmalloc
