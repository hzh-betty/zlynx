#ifndef ZMALLOC_CENTRAL_CACHE_H_
#define ZMALLOC_CENTRAL_CACHE_H_

/**
 * @file central_cache.h
 * @brief 中心缓存，跨线程共享，使用桶锁同步
 */

#include "common.h"

namespace zmalloc {

// 前向声明
class PageCache;

/**
 * @brief 中心缓存（单例）
 *
 * 作为 ThreadCache 和 PageCache 之间的中间层。
 * 每个桶有独立的锁，减少锁竞争。
 */
class CentralCache {
public:
  /**
   * @brief 获取单例实例
   */
  static CentralCache &get_instance() {
    static CentralCache instance;
    return instance;
  }

  /**
   * @brief 从 CentralCache 获取一定数量的对象给 ThreadCache
   * @param start 输出起始指针
   * @param end 输出结束指针
   * @param n 请求数量
   * @param size 对象大小
   * @return 实际获取数量
   */
  size_t fetch_range_obj(void *&start, void *&end, size_t n, size_t size);

  /**
   * @brief 获取一个非空的 Span
   * @param span_list Span 链表
   * @param size 对象大小
   * @return Span 指针
   */
  Span *get_one_span(SpanList &span_list, size_t size);

  /**
   * @brief 将对象链表归还给对应的 Span
   * @param start 链表起始
   * @param size 对象大小
   */
  void release_list_to_spans(void *start, size_t size);

private:
  CentralCache() = default;
  CentralCache(const CentralCache &) = delete;
  CentralCache &operator=(const CentralCache &) = delete;

private:
  SpanList span_lists_[NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_CENTRAL_CACHE_H_
