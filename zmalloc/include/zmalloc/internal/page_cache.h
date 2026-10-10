/**
 * @file page_cache.h
 * @brief 页缓存，以页为单位管理内存，支持 Span 的分配、合并和释放
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_PAGE_CACHE_H_
#define ZMALLOC_INTERNAL_PAGE_CACHE_H_

#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>

#include <new>

#include "common.h"
#include "object_pool.h"
#include "page_map.h"
#include "span_list.h"
#include "zmalloc_config.h"

namespace zmalloc {

/** @brief 受页锁保护的页缓存统计，不包含元数据映射。 */
struct PageCacheStats {
    size_t free_bytes;
    size_t released_bytes;
    size_t mapped_bytes;
    size_t total_released_bytes;
};

/**
 * @brief 页缓存（单例）
 *
 * PageCache 是分配器的页级后端：向系统申请连续页，再按请求页数
 * 切分为 Span。Span 回收时与相邻空闲 Span 合并，以减少外碎片。
 *
 * 空闲 Span 按页数放入 span_lists_。已使用 Span 的每一页通过
 * PageMap 映射回所属 Span。超过缓存桶上限的大 Span 直接向系统
 * 申请；不超过单块上限的空闲大 Span 在独立预算内按精确尺寸复用。
 *
 * @note 修改页缓存状态时，调用者必须持有 page_mtx_。
 */
class PageCache : public NonCopyable {
  public:
    /**
     * @brief 获取单例实例
     */
    static PageCache &get_instance() {
        // 全局 malloc/free 在静态析构期间仍可能调用；底层缓存保留到进程结束。
        alignas(PageCache) static unsigned char storage[sizeof(PageCache)];
        static PageCache *instance = new (storage) PageCache;
        return *instance;
    }

    /**
     * @brief 获取 k 页的 Span
     * @param k 页数
     * @return Span 指针
     */
    Span *new_span(size_t k);

    /**
     * @brief 根据对象地址获取对应的 Span
     * @param obj 对象指针
     * @return Span 指针
     * @note 仅用于存活的受管对象，或持有 page_mtx_ 的内部访问。小对象及
     *       尚未归还中心层的缓存对象计入 use_count，保证所属 Span 不被复用。
     */
    inline Span *map_object_to_span(void *obj) {
        PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
        Span *ret = decode_span(id_span_map_.get(id));
        assert(ret != nullptr);
        return ret;
    }

    /**
     * @brief 尝试根据任意地址查询 Span，允许映射缺失
     * @param obj 对象指针
     * @return 找到则返回 Span，否则返回 nullptr
     * @note 原子查询不锁定 Span 生命周期。任意地址的查询结果若需解引用，
     *       必须在查询前取得 page_mtx_ 并保持到检查结束；存活对象可无锁查询。
     */
    inline Span *try_map_object_to_span(void *obj) {
        PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
        return decode_span(id_span_map_.get(id));
    }

    /**
     * @brief 查询带缓存映射标记的 Span，供 override 的无锁常见路径使用。
     * @note 缓存映射不会 munmap，合法外部指针不可能落入这些页。
     *       合法受管指针自身保证 Span 存活；非法释放仍不在本接口的保证范围内。
     *       若未来解除缓存映射，必须同步调整此标记及查询的生命周期协议。
     */
    Span *try_map_cached_object_to_span(void *obj) {
        const PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
        void *entry = id_span_map_.get(id);
        return (reinterpret_cast<uintptr_t>(entry) & kCachedObjectTag) != 0
                   ? decode_span(entry)
                   : nullptr;
    }

    /** @brief 登记大对齐块的附加页；调用者须持有 page_mtx_。 */
    void map_span_page(Span *span, void *address) {
        const PageId id = reinterpret_cast<PageId>(address) >> PAGE_SHIFT;
        assert(id >= span->page_id && id - span->page_id < span->n);
        id_span_map_.set(id, span);
    }

    /**
     * @brief 释放 Span 到 PageCache，并尝试合并相邻 Span
     * @param span 要释放的 Span
     */
    void release_span_to_page_cache(Span *span);

    /** @brief 建议回收完全空闲 Span 的物理页；调用者须持有 page_mtx_。 */
    size_t release_free_pages();

    /** @brief 获取页缓存统计；调用者须持有 page_mtx_。 */
    PageCacheStats statistics();

    /** @brief 返回保护页缓存元数据的互斥锁；调用者负责加锁和解锁。 */
    std::mutex &page_mtx() { return page_mtx_; }

  private:
    static_assert(alignof(Span) >= 2, "Span must reserve a low bit for tagging");
    static constexpr uintptr_t kCachedObjectTag = 1;

    // 标记保存在原子叶项的低位；所有通用查询和合并路径须先去除标记。
    static Span *decode_span(void *entry) {
        return reinterpret_cast<Span *>(
            reinterpret_cast<uintptr_t>(entry) & ~kCachedObjectTag);
    }

    // 缓存中的已分配 Span 带标记，空闲边界和直接系统映射均不带标记。
    void map_cached_span(Span *span) {
        assert(span->n <= NPAGES - 1 && span->is_use);
        void *entry = reinterpret_cast<void *>(
            reinterpret_cast<uintptr_t>(span) | kCachedObjectTag);
        id_span_map_.set_range(span->page_id, span->n, entry);
    }

    // 游标指向的空闲 Span 被分配、合并或驱逐前，先移到同桶下一个节点。
    void advance_release_cursor(Span *span) {
        if (auto_release_cursor_ == span) {
            auto_release_cursor_ = span->prev;
        }
    }
    void maybe_release_free_pages(size_t returned_bytes);

    PageCache() = default;

  private:
    SpanList large_spans_;        // 精确尺寸复用，按归还顺序驱逐，页锁保护。
    size_t large_cached_bytes_ = 0;
    std::array<SpanList, NPAGES> span_lists_; // 按页数分桶
    PageMap id_span_map_;         // 页号到 Span 的映射
    ObjectPool<Span> span_pool_;  // Span 对象池
    std::mutex page_mtx_;         // 全局锁
    size_t mapped_bytes_ = 0;     // 受管页映射，不含元数据；由页锁保护。
    size_t total_released_bytes_ = 0; // 累计成功建议回收字节数。
    size_t unreleased_free_bytes_ = 0;
    size_t returned_since_auto_release_ = 0;
    size_t auto_release_bucket_ = 0; // 0 为大块链表，1..128 为普通桶。
    Span *auto_release_cursor_ = nullptr;
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_PAGE_CACHE_H_
