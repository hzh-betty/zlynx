/**
 * @file page_heap.h
 * @brief 页堆管理
 *
 * PageHeap 是内存池的后端，负责：
 * 1. 从系统申请大块内存
 * 2. 管理空闲 Span
 * 3. 映射页号到 Span
 */

#ifndef ZMALLOC_PAGE_HEAP_H_
#define ZMALLOC_PAGE_HEAP_H_

#include <mutex>
#include <unordered_map>

#include "common.h"
#include "span.h"

namespace zmalloc {

/// 最大管理页数 (128 页)
static constexpr size_t kMaxPages = 128;

/**
 * @class PageHeap
 * @brief 页堆管理器（单例）
 */
class PageHeap {
public:
  /// 获取单例实例
  static PageHeap &Instance();

  /**
   * @brief 申请指定页数的 Span
   * @param num_pages 请求的页数
   * @return 分配的 Span，失败返回 nullptr
   */
  Span *AllocateSpan(size_t num_pages);

  /**
   * @brief 归还 Span
   * @param span 要归还的 Span
   *
   * 会尝试与相邻空闲 Span 合并以减少碎片。
   */
  void DeallocateSpan(Span *span);

  /**
   * @brief 根据页号查找对应的 Span
   * @param page_id 页号
   * @return 对应的 Span，未找到返回 nullptr
   */
  Span *GetSpanByPageId(size_t page_id);

private:
  PageHeap() = default;
  PageHeap(const PageHeap &) = delete;
  PageHeap &operator=(const PageHeap &) = delete;

  /// 从系统申请新的内存
  Span *AllocateFromSystem(size_t num_pages);

  std::mutex mutex_;

  /// 按页数分桶的空闲 Span 链表
  SpanList free_lists_[kMaxPages + 1];

  /// 页号到 Span 的映射 (简化版，实际可用基数树)
  std::unordered_map<size_t, Span *> page_map_;
};

} // namespace zmalloc

#endif // ZMALLOC_PAGE_HEAP_H_
