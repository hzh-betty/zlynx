/**
 * @file span.h
 * @brief Span 内存块管理
 *
 * Span 表示连续 N 页的内存块，是 PageHeap 和 CentralCache 的基本单位。
 */

#ifndef ZMALLOC_SPAN_H_
#define ZMALLOC_SPAN_H_

#include "common.h"

namespace zmalloc {

/**
 * @class Span
 * @brief 连续页的内存块
 *
 * Span 可以处于两种状态：
 * 1. 未分割：直接用于大对象分配
 * 2. 已分割：切分为多个小对象，由 CentralCache 管理
 */
class Span {
public:
  Span() = default;

  /// 获取起始页号
  size_t PageId() const { return page_id_; }
  void SetPageId(size_t page_id) { page_id_ = page_id; }

  /// 获取页数
  size_t NumPages() const { return num_pages_; }
  void SetNumPages(size_t num_pages) { num_pages_ = num_pages; }

  /// 获取起始地址
  void *StartAddress() const {
    return reinterpret_cast<void *>(page_id_ << kPageShift);
  }

  /// 获取总字节数
  size_t TotalBytes() const { return PagesToSize(num_pages_); }

  /// 空闲对象链表操作
  void *PopFreeObject();
  void PushFreeObject(void *obj);
  bool HasFreeObject() const { return free_list_ != nullptr; }

  /// 使用计数
  size_t UseCount() const { return use_count_; }
  void IncrementUseCount() { ++use_count_; }
  void DecrementUseCount() { --use_count_; }
  bool IsFullyFree() const { return use_count_ == 0; }

  /// SizeClass 信息
  size_t SizeClassIndex() const { return size_class_; }
  void SetSizeClass(size_t class_index) { size_class_ = class_index; }

  /// 链表指针（用于 SpanList）
  Span *next = nullptr;
  Span *prev = nullptr;

private:
  size_t page_id_ = 0;        ///< 起始页号 (地址 >> kPageShift)
  size_t num_pages_ = 0;      ///< 包含的页数
  void *free_list_ = nullptr; ///< 空闲对象链表
  size_t use_count_ = 0;      ///< 已分配对象数
  size_t size_class_ = 0;     ///< 被分割的 SizeClass (0 表示未分割)
};

/**
 * @class SpanList
 * @brief 双向链表管理 Span
 */
class SpanList {
public:
  SpanList();

  /// 插入到链表头部
  void PushFront(Span *span);

  /// 移除指定 Span
  void Erase(Span *span);

  /// 弹出头部 Span
  Span *PopFront();

  /// 链表是否为空
  bool IsEmpty() const { return head_.next == &head_; }

  /// 获取头部 Span（不移除）
  Span *Front() const { return IsEmpty() ? nullptr : head_.next; }

private:
  Span head_; ///< 哨兵节点
};

} // namespace zmalloc

#endif // ZMALLOC_SPAN_H_
