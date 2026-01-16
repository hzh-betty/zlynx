/**
 * @file central_cache.h
 * @brief 中央缓存管理
 *
 * CentralCache 是 ThreadCache 和 PageHeap 之间的桥梁：
 * 1. 按 SizeClass 组织 Span
 * 2. 为 ThreadCache 提供批量分配
 * 3. 回收 ThreadCache 归还的对象
 */

#ifndef ZMALLOC_CENTRAL_CACHE_H_
#define ZMALLOC_CENTRAL_CACHE_H_

#include <mutex>

#include "common.h"
#include "span.h"

namespace zmalloc {

/**
 * @class CentralCache
 * @brief 中央缓存（单例）
 *
 * 每个 SizeClass 使用独立的锁，减少锁竞争。
 */
class CentralCache {
public:
  /// 获取单例实例
  static CentralCache &Instance();

  /**
   * @brief 批量获取对象
   * @param class_index SizeClass 索引
   * @param batch_count 期望获取的数量
   * @return 对象链表头指针，失败返回 nullptr
   *
   * 实际返回数量可能小于 batch_count。
   */
  void *FetchRange(size_t class_index, size_t batch_count,
                   size_t *actual_count);

  /**
   * @brief 批量归还对象
   * @param class_index SizeClass 索引
   * @param start 链表起始指针
   * @param count 对象数量
   */
  void ReleaseRange(size_t class_index, void *start, size_t count);

private:
  CentralCache() = default;
  CentralCache(const CentralCache &) = delete;
  CentralCache &operator=(const CentralCache &) = delete;

  /// 从 PageHeap 获取新的 Span
  Span *GetSpanFromPageHeap(size_t class_index);

  /// 每个 SizeClass 的锁
  std::mutex mutexes_[kNumSizeClasses];

  /// 每个 SizeClass 的 Span 链表
  SpanList span_lists_[kNumSizeClasses];
};

} // namespace zmalloc

#endif // ZMALLOC_CENTRAL_CACHE_H_
