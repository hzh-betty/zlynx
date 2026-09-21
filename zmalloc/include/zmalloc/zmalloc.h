/**
 * @file zmalloc.h
 * @brief zmalloc 对外统一接口
 * @author hzh-betty
 *
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
    // 第一步：零字节请求按接口约定直接返回空指针。
    if (ZM_UNLIKELY(size == 0)) {
        return nullptr;
    }

    if (ZM_LIKELY(size <= MAX_BYTES)) {
        // 第二步：小对象进入当前线程缓存，这是最常见的无锁路径。
        return get_thread_cache()->allocate(size);
    }

    // 第三步：大对象按页向 PageCache 申请，记录原始请求大小供释放时分流。
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
    // 第一步：free(nullptr) 无需处理。
    if (ZM_UNLIKELY(ptr == nullptr)) {
        return;
    }

    PageCache &pc = PageCache::get_instance();
    Span *span = pc.map_object_to_span(ptr);
    const size_t size = span->obj_size;

    if (ZM_LIKELY(size <= MAX_BYTES)) {
        // 第二步：小对象回到当前线程缓存，后续可能批量流向共享缓存。
        get_thread_cache()->deallocate(ptr, size);
        return;
    }

    // 第三步：大对象整段归还 PageCache；超大 Span 会进一步归还系统。
    pc.page_mtx().lock();
    pc.release_span_to_page_cache(span);
    pc.page_mtx().unlock();
}

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
