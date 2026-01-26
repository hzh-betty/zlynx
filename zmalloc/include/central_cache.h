#ifndef ZMALLOC_CENTRAL_CACHE_H_
#define ZMALLOC_CENTRAL_CACHE_H_

/**
 * @file central_cache.h
 * @brief 中心缓存，跨线程共享，使用桶锁同步
 */

#include "common.h"
#include "zmalloc_noncopyable.h"

namespace zmalloc {

// 前向声明
class PageCache;

/**
 * @brief 中心缓存（单例）
 *
 * 作为 ThreadCache 和 PageCache 之间的中间层。
 * 每个桶有独立的锁，减少锁竞争。
 */
class CentralCache : public NonCopyable {
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
   * @brief 与 fetch_range_obj 相同，但调用方已计算好 size class 索引
   * @note 这是 ThreadCache 热路径的优化重载，避免重复的 size->index 映射。
   */
  size_t fetch_range_obj(void *&start, void *&end, size_t n, size_t size,
                         size_t index);

  /**
   * @brief 获取一个非空的 Span
   *
   * - 每个 sizeclass 维护两条 SpanList：
   *   - nonempty：至少还有一个可分配对象（span->free_list != nullptr）
   *   - empty：已无可分配对象（span->free_list ==
   * nullptr），但仍有对象在外部(ThreadCache)持有
   * - 这样 fetch 不需要线性扫描，通常 O(1) 取到可用 span。
   * - 同一 sizeclass 的 nonempty/empty 由同一把锁保护。
   * @param size 对象大小
   * @return Span 指针
   */
  struct alignas(64) CentralFreeList {
    SpanList nonempty;
    SpanList empty;
    // 保护 nonempty/empty 以及 span 在两者之间的迁移。
    SpinLock lock;
  };

  Span *get_one_span(CentralFreeList &free_list, size_t size);

  /**
   * @brief 将对象链表归还给对应的 Span
   * @param start 链表起始
   * @param size 对象大小
   */
  void release_list_to_spans(void *start, size_t size);

  /**
   * @brief 与 release_list_to_spans 相同，但调用方已计算好 size class 索引
   */
  void release_list_to_spans(void *start, size_t size, size_t index);

private:
  CentralCache() = default;

private:
  CentralFreeList free_lists_[NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_CENTRAL_CACHE_H_
