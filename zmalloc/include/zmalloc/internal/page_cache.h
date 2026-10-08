/**
 * @file page_cache.h
 * @brief 页缓存，以页为单位管理内存，支持 Span 的分配、合并和释放
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_PAGE_CACHE_H_
#define ZMALLOC_INTERNAL_PAGE_CACHE_H_

#include <array>
#include <cassert>
#include <mutex>

#include <new>

#include "common.h"
#include "object_pool.h"
#include "page_map.h"
#include "span_list.h"
#include "zmalloc_config.h"

namespace zmalloc {

/**
 * @brief 页缓存（单例）
 *
 * PageCache 是分配器的页级后端：向系统申请连续页，再按请求页数
 * 切分为 Span。Span 回收时与相邻空闲 Span 合并，以减少外碎片。
 *
 * 空闲 Span 按页数放入 span_lists_。已使用 Span 的每一页通过
 * PageMap 映射回所属 Span。超过缓存桶上限的大 Span 直接向系统
 * 申请和归还。
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
     */
    inline Span *map_object_to_span(void *obj) {
        PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
        Span *ret = static_cast<Span *>(id_span_map_.get(id));
        assert(ret != nullptr);
        return ret;
    }

    /**
     * @brief 安全尝试根据对象地址获取 Span
     * @param obj 对象指针
     * @return 找到则返回 Span，否则返回 nullptr
     */
    inline Span *try_map_object_to_span(void *obj) {
        PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
        return static_cast<Span *>(id_span_map_.get(id));
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

    /** @brief 返回保护页缓存元数据的互斥锁；调用者负责加锁和解锁。 */
    std::mutex &page_mtx() { return page_mtx_; }

  private:
    PageCache() = default;

  private:
    std::array<SpanList, NPAGES> span_lists_; // 按页数分桶
    PageMap id_span_map_;         // 页号到 Span 的映射
    ObjectPool<Span> span_pool_;  // Span 对象池
    std::mutex page_mtx_;         // 全局锁
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_PAGE_CACHE_H_
