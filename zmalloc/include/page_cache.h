#ifndef ZMALLOC_PAGE_CACHE_H_
#define ZMALLOC_PAGE_CACHE_H_

/**
 * @file page_cache.h
 * @brief 页缓存，以页为单位管理内存，支持 Span 的分配、合并和释放
 */

#include <mutex>

#include "common.h"
#include "object_pool.h"
#include "page_map.h"
#include "zmalloc_noncopyable.h"

namespace zmalloc {

/**
 * @brief 页缓存（单例）
 *
 * 管理 Span 的分配和回收，支持 Span 的合并以减少外碎片。
 */
class PageCache : public NonCopyable {
public:
  /**
   * @brief 获取单例实例
   */
  static PageCache &get_instance() {
    static PageCache instance;
    return instance;
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
   * @brief 释放 Span 到 PageCache，并尝试合并相邻 Span
   * @param span 要释放的 Span
   */
  void release_span_to_page_cache(Span *span);

  /**
   * @brief 获取页级别锁
   */
  std::mutex &page_mtx() { return page_mtx_; }

  /**
   * @brief 快速获取对象的 size class（不加载 Span）
   * @param obj 对象指针
   * @return size class，0 表示大对象
   */
  inline uint8_t get_sizeclass(void *obj) const {
    PageId id = reinterpret_cast<PageId>(obj) >> PAGE_SHIFT;
    return id_span_map_.sizeclass(id);
  }

  /**
   * @brief 设置页映射并同时设置 size class
   * @param page_id 起始页号
   * @param n 页数
   * @param span Span 指针
   * @param sc size class
   */
  inline void set_range_with_sizeclass(PageId page_id, size_t n, Span *span,
                                       uint8_t sc) {
    id_span_map_.set_range_with_sizeclass(page_id, n, span, sc);
  }

  /**
   * @brief 只设置 size class，不覆盖 Span 映射
   * @param page_id 起始页号
   * @param n 页数
   * @param sc size class
   */
  inline void set_range_sizeclass_only(PageId page_id, size_t n, uint8_t sc) {
    id_span_map_.set_range_sizeclass_only(page_id, n, sc);
  }

private:
  PageCache() = default;

private:
  SpanList span_lists_[NPAGES]; // 按页数分桶
  PageMap id_span_map_;         // 页号到 Span 的映射
  ObjectPool<Span> span_pool_;  // Span 对象池
  std::mutex page_mtx_;         // 全局锁
};

} // namespace zmalloc

#endif // ZMALLOC_PAGE_CACHE_H_
