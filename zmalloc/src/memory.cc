/**
 * @file memory.cc
 * @brief 显式缓存清理、物理页回收与内存统计
 */

#include "zmalloc/zmalloc.h"

#include "zmalloc/internal/transfer_cache.h"

namespace zmalloc {

size_t release_memory() {
    // TLS 注册或异常处理可能调用 malloc，重入时使用 bootstrap 分配器。
    internal::AllocatorCallGuard guard;
    get_thread_cache()->cleanup();
    TransferCache::get_instance().drain();
    PageCache &pc = PageCache::get_instance();
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    return pc.release_free_pages();
}

MemoryStats memory_stats() {
    internal::AllocatorCallGuard guard;
    MemoryStats stats{};
    stats.thread_cache_bytes = get_thread_cache()->cached_bytes();
    stats.transfer_cache_bytes = TransferCache::get_instance().cached_bytes();
    PageCache &pc = PageCache::get_instance();
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    const PageCacheStats pages = pc.statistics();
    stats.page_cache_free_bytes = pages.free_bytes;
    stats.page_cache_released_bytes = pages.released_bytes;
    stats.page_cache_mapped_bytes = pages.mapped_bytes;
    stats.total_released_bytes = pages.total_released_bytes;
    return stats;
}

} // namespace zmalloc
