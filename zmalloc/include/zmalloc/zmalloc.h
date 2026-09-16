/**
 * @file zmalloc.h
 * @brief zmalloc 对外统一接口
 * @author hzh-betty
 *
 * 提供高性能的内存分配和释放 API。
 */

#ifndef ZMALLOC_ZMALLOC_H_
#define ZMALLOC_ZMALLOC_H_

#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/size_class.h"
#include "zmalloc/internal/thread_cache.h"
#include "zmalloc/internal/zmalloc_config.h"

namespace zmalloc {

/**
 * @brief 分配内存
 * @param size 请求字节数
 * @return 内存指针，失败抛出 std::bad_alloc
 */
ZM_ALWAYS_INLINE void *zmalloc(size_t size) {
    if (ZM_UNLIKELY(size == 0)) {
        return nullptr;
    }

    if (ZM_LIKELY(size <= MAX_BYTES)) {
        return get_thread_cache()->allocate(size);
    }

    // 冷路径：大对象分配
    size_t k_page = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    PageCache &pc = PageCache::get_instance();
    pc.page_mtx().lock();
    Span *span = pc.new_span(k_page);
    span->is_use = true;
    span->obj_size = size;
    pc.page_mtx().unlock();
    return reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
}

/**
 * @brief 释放内存
 * @param ptr 内存指针
 */
ZM_ALWAYS_INLINE void zfree(void *ptr) {
    if (ZM_UNLIKELY(ptr == nullptr)) {
        return;
    }

    PageCache &pc = PageCache::get_instance();
    Span *span = pc.map_object_to_span(ptr);
    const size_t size = span->obj_size;

    if (ZM_LIKELY(size <= MAX_BYTES)) {
        get_thread_cache()->deallocate(ptr, size);
        return;
    }

    // 冷路径：大对象释放
    pc.page_mtx().lock();
    pc.release_span_to_page_cache(span);
    pc.page_mtx().unlock();
}

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
