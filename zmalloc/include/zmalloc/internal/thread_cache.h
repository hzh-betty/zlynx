/**
 * @file thread_cache.h
 * @brief 线程本地缓存，每个线程独享，无锁快速分配小对象
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_THREAD_CACHE_H_
#define ZMALLOC_INTERNAL_THREAD_CACHE_H_

#include "common.h"
#include "zmalloc/internal/free_list.h"
#include "zmalloc/internal/size_class.h"
#include "zmalloc_config.h"

namespace zmalloc {

class ThreadCache : public NonCopyable {
  public:
    // 释放路径触发的软预算；分配补货及低水位回收不保证立即低于预算。
    static constexpr size_t kCacheBudget = 1024 * 1024;

    ZM_ALWAYS_INLINE void *allocate(size_t size) {
        const auto &e = SizeClass::lookup(size);
        assert(size > 0 && size <= MAX_BYTES);
        FreeList &list = free_lists_[e.index];
        if (ZM_LIKELY(!closed_ && !list.empty())) {
            cached_bytes_ -= e.align_size;
            return list.pop();
        }
        return fetch_from_central_cache(e);
    }

    ZM_ALWAYS_INLINE void deallocate(void *ptr, size_t size) {
        const auto &e = SizeClass::lookup(size);
        assert(ptr && size > 0 && size <= MAX_BYTES);
        if (ZM_UNLIKELY(closed_)) {
            return release_direct(ptr, e);
        }
        class_sizes_[e.index] = e.align_size;
        FreeList &list = free_lists_[e.index];
        list.push(ptr);
        cached_bytes_ += e.align_size;
        if (ZM_UNLIKELY(list.size() > list.max_size() ||
                        cached_bytes_ > kCacheBudget)) {
            deallocate_slow(e);
        }
    }

    // 仅归还空闲对象，不影响仍由调用方持有的对象。可重复调用。
    void cleanup();
    // 退出钩子先关闭缓存，后续 TLS 析构中的分配/释放直接走 CentralCache。
    void shutdown();
    size_t cached_bytes() const { return cached_bytes_; }

  private:
    void *fetch_from_central_cache(const SizeClassLookup &e);
    void release_direct(void *ptr, const SizeClassLookup &e);
    void release_batch(size_t index, size_t count, bool use_transfer = true);
    void deallocate_slow(const SizeClassLookup &e);
    void scavenge();

    FreeList free_lists_[NFREELISTS];
    size_t class_sizes_[NFREELISTS] = {};
    unsigned length_overages_[NFREELISTS] = {};
    size_t cached_bytes_ = 0;
    bool closed_ = false;
};

ThreadCache *get_thread_cache();

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_THREAD_CACHE_H_
