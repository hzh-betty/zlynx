#ifndef ZMALLOC_ZMALLOC_H_
#define ZMALLOC_ZMALLOC_H_

/**
 * @file zmalloc.h
 * @brief zmalloc 对外统一接口
 *
 * 提供高性能的内存分配和释放 API。
 */

#include "common.h"
#include "page_cache.h"
#include "thread_cache.h"

namespace zmalloc {

/**
 * @brief 分配内存
 * @param size 请求字节数
 * @return 内存指针，失败抛出 std::bad_alloc
 */
inline void *zmalloc(size_t size) {
  if (size == 0) {
    return nullptr;
  }

  if (size > MAX_BYTES) {
    // 大于 256KB 直接向 PageCache 申请
    size_t align_size = SizeClass::round_up(size);
    size_t k_page = align_size >> PAGE_SHIFT;

    PageCache::get_instance().page_mtx().lock();
    Span *span = PageCache::get_instance().new_span(k_page);
    span->is_use = true;
    span->obj_size = size;
    PageCache::get_instance().page_mtx().unlock();

    void *ptr = reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
    return ptr;
  } else {
    return get_thread_cache()->allocate(size);
  }
}

/**
 * @brief 释放内存
 * @param ptr 内存指针
 */
inline void zfree(void *ptr) {
  if (ptr == nullptr) {
    return;
  }

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  size_t size = span->obj_size;

  if (size > MAX_BYTES) {
    PageCache::get_instance().page_mtx().lock();
    PageCache::get_instance().release_span_to_page_cache(span);
    PageCache::get_instance().page_mtx().unlock();
  } else {
    get_thread_cache()->deallocate(ptr, size);
  }
}

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
