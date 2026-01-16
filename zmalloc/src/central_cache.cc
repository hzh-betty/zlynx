/**
 * @file central_cache.cc
 * @brief CentralCache 实现
 */

#include "central_cache.h"
#include "page_cache.h"

namespace zmalloc {

size_t CentralCache::fetch_range_obj(void *&start, void *&end, size_t n,
                                     size_t size) {
  size_t index = SizeClass::index(size);
  span_lists_[index].mtx.lock();

  // 获取一个非空的 Span
  Span *span = get_one_span(span_lists_[index], size);
  assert(span);
  assert(span->free_list);

  // 从 Span 获取 n 个对象，不够则有多少拿多少
  start = span->free_list;
  end = span->free_list;
  size_t actual_num = 1;
  while (next_obj(end) && n - 1) {
    end = next_obj(end);
    ++actual_num;
    --n;
  }
  span->free_list = next_obj(end);
  next_obj(end) = nullptr;
  span->use_count += actual_num;

  span_lists_[index].mtx.unlock();
  return actual_num;
}

Span *CentralCache::get_one_span(SpanList &span_list, size_t size) {
  // 1. 先在 span_list 中寻找非空的 Span
  Span *it = span_list.begin();
  while (it != span_list.end()) {
    if (it->free_list != nullptr) {
      return it;
    }
    it = it->next;
  }

  // 2. 没有非空 Span，向 PageCache 申请
  // 先解桶锁，让其他线程释放内存不阻塞
  span_list.mtx.unlock();

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
  char *end = start + bytes;
  span->free_list = start;
  start += size;
  void *tail = span->free_list;

  while (start < end) {
    next_obj(tail) = start;
    tail = next_obj(tail);
    start += size;
  }
  next_obj(tail) = nullptr;

  // 重新加桶锁，挂到 span_list
  span_list.mtx.lock();
  span_list.push_front(span);

  return span;
}

void CentralCache::release_list_to_spans(void *start, size_t size) {
  size_t index = SizeClass::index(size);
  span_lists_[index].mtx.lock();

  while (start) {
    void *next = next_obj(start);
    Span *span = PageCache::get_instance().map_object_to_span(start);

    // 头插到 Span 的自由链表
    next_obj(start) = span->free_list;
    span->free_list = start;

    --span->use_count;
    if (span->use_count == 0) {
      // Span 所有对象都回来了，归还给 PageCache
      span_lists_[index].erase(span);
      span->free_list = nullptr;
      span->next = nullptr;
      span->prev = nullptr;

      span_lists_[index].mtx.unlock();
      PageCache::get_instance().page_mtx().lock();
      PageCache::get_instance().release_span_to_page_cache(span);
      PageCache::get_instance().page_mtx().unlock();
      span_lists_[index].mtx.lock();
    }

    start = next;
  }

  span_lists_[index].mtx.unlock();
}

} // namespace zmalloc
