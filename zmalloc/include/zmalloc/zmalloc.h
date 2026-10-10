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

#include <limits>

namespace zmalloc {

/**
 * @brief 缓存及页内存统计，不含 PageMap/ObjectPool 等元数据。
 * @note 线程缓存仅统计调用线程；共享缓存按大小类分别取样，整体不是原子快照。
 *       released 字节表示成功建议回收的页范围，不代表实际 RSS 降幅。
 */
struct MemoryStats {
    size_t thread_cache_bytes;        // 当前线程缓存的空闲对象字节数。
    size_t transfer_cache_bytes;      // 共享传输缓存的空闲对象字节数。
    size_t page_cache_free_bytes;     // 页缓存中完全空闲的页字节数。
    size_t page_cache_released_bytes; // 其中整段已建议回收的空闲 Span 字节数。
    size_t page_cache_mapped_bytes;   // 受管页的虚拟映射字节数，含直接分配的大块。
    size_t total_released_bytes;      // 累计成功建议回收的字节数，重复复用后可再次计入。
};

/**
 * @brief 清理当前线程缓存、有界排空传输缓存，并回收完全空闲页的物理内存。
 * @return 本次成功 MADV_DONTNEED 的页范围字节数；保留虚拟地址映射。
 * @note 不访问其他线程的 TLS；并发分配时不保证共享缓存最终为空。
 *       可重复调用，调用后仍可正常分配；首次初始化可能抛出 std::bad_alloc。
 */
size_t release_memory();

/** @brief 返回内存统计；首次初始化可能抛出 std::bad_alloc。 */
MemoryStats memory_stats();

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
        return internal::get_thread_cache_fast()->allocate(size);
    }

    // 第三步：大对象按页向 PageCache 申请，记录原始请求大小供释放时分流。
    if (size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
        throw std::bad_alloc();
    }
    size_t k_page = ((size - 1) >> PAGE_SHIFT) + 1;
    PageCache &pc = PageCache::get_instance();
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    Span *span = pc.new_span(k_page);
    span->is_use = true;
    span->obj_size = size;
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
        internal::get_thread_cache_fast()->deallocate(ptr, size);
        return;
    }

    // 第三步：大对象整段归还 PageCache；超大 Span 会进一步归还系统。
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    pc.release_span_to_page_cache(span);
}

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
