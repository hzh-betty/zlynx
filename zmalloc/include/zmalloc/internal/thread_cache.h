/**
 * @file thread_cache.h
 * @brief 线程本地缓存，每个线程独享，无锁快速分配小对象
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_THREAD_CACHE_H_
#define ZMALLOC_INTERNAL_THREAD_CACHE_H_

#include <array>

#include "common.h"
#include "zmalloc/internal/free_list.h"
#include "zmalloc/internal/size_class.h"
#include "zmalloc_config.h"

namespace zmalloc {

/**
 * @brief 每线程独享的小对象一级缓存。
 *
 * 每个大小类维护一条 FreeList。分配命中时只操作当前线程的数据，
 * 不需要加锁；未命中时先从 TransferCache 批量取对象，再回退到
 * CentralCache。释放的对象先留在本线程，超过动态阈值或总字节
 * 预算后再批量归还。
 *
 * ThreadCache 的生命周期与线程绑定。线程退出时，cleanup 钩子
 * 归还空闲对象。closed_ 用于避免 TLS 析构阶段再次向本地缓存补货。
 *
 * @note 实例只能由所属线程访问，类本身不提供并发保护。
 */
class ThreadCache : public NonCopyable {
  public:
    // 释放路径触发的软预算；分配补货及低水位回收不保证立即低于预算。
    static constexpr size_t kCacheBudget = THREAD_CACHE_BUDGET;

    /** @brief 为当前线程分配一个小对象；缓存未命中时向共享缓存批量补货。 */
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

    /** @brief 将小对象放回当前线程缓存，超出阈值时触发回收。 */
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

    /** @brief 归还本线程的空闲对象；不影响调用方持有的对象，可重复调用。 */
    void cleanup();
    /** @brief 关闭本地缓存并清理空闲对象；之后的分配/释放直接走共享缓存。 */
    void shutdown();
    /** @brief 返回当前线程缓存中空闲对象占用的字节数。 */
    size_t cached_bytes() const { return cached_bytes_; }

  private:
    /** @brief 从共享缓存补充一批对象，并返回其中一个供当前请求使用。 */
    void *fetch_from_central_cache(const SizeClassLookup &e);
    /** @brief 将单个对象直接归还中心缓存，供缓存关闭后的释放路径使用。 */
    void release_direct(void *ptr, const SizeClassLookup &e);
    /** @brief 从本地链表取出一批对象并归还共享缓存。 */
    void release_batch(size_t index, size_t count, bool use_transfer = true);
    /** @brief 处理一次释放触发的批量回收和缓存预算检查。 */
    void deallocate_slow(const SizeClassLookup &e);
    /** @brief 按各大小类低水位回收长期闲置的本地对象。 */
    void scavenge();

    std::array<FreeList, NFREELISTS> free_lists_;
    std::array<size_t, NFREELISTS> class_sizes_{};
    std::array<unsigned, NFREELISTS> length_overages_{};
    size_t cached_bytes_ = 0;
    bool closed_ = false;
};

/** @brief 获取当前线程专属的 ThreadCache，并注册线程退出清理钩子。 */
ThreadCache *get_thread_cache();

namespace internal {

#if defined(__GNUC__) || defined(__clang__)
// 常量初始化的原始 TLS 指针，避免 C++ 动态 TLS 包装器再次进入分配器。
extern __thread ThreadCache *tls_thread_cache_ptr
    __attribute__((tls_model("initial-exec")));
#endif

ZM_ALWAYS_INLINE ThreadCache *get_thread_cache_fast() {
#if defined(__GNUC__) || defined(__clang__)
    if (ZM_LIKELY(tls_thread_cache_ptr != nullptr)) {
        return tls_thread_cache_ptr;
    }
#endif
    // 第一次访问仍走既有注册协议；晚期析构返回同一个已关闭的缓存。
    return get_thread_cache();
}

} // namespace internal

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_THREAD_CACHE_H_
