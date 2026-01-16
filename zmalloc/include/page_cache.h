#ifndef ZMALLOC_PAGE_CACHE_H_
#define ZMALLOC_PAGE_CACHE_H_

/**
 * @file page_cache.h
 * @brief 页缓存，以页为单位管理内存，支持 Span 的分配、合并和释放
 */

#include "common.h"
#include "object_pool.h"
#include "page_map.h"

namespace zmalloc {

/**
 * @brief 页缓存（单例）
 *
 * 管理 Span 的分配和回收，支持 Span 的合并以减少外碎片。
 */
class PageCache {
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
  Span *map_object_to_span(void *obj);

  /**
   * @brief 释放 Span 到 PageCache，并尝试合并相邻 Span
   * @param span 要释放的 Span
   */
  void release_span_to_page_cache(Span *span);

  /**
   * @brief 获取页级别锁
   */
  std::mutex &page_mtx() { return page_mtx_; }

private:
  PageCache() = default;
  PageCache(const PageCache &) = delete;
  PageCache &operator=(const PageCache &) = delete;

private:
  SpanList span_lists_[NPAGES]; // 按页数分桶
  PageMap id_span_map_;         // 页号到 Span 的映射
  ObjectPool<Span> span_pool_;  // Span 对象池
  std::mutex page_mtx_;         // 全局锁
};

} // namespace zmalloc

#endif // ZMALLOC_PAGE_CACHE_H_
