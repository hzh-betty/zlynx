/**
 * @file thread_cache.h
 * @brief 线程本地缓存
 *
 * ThreadCache 是内存池的前端，为每个线程提供私有的缓存：
 * 1. 无锁分配/释放（最热路径）
 * 2. 超出阈值时批量归还给 CentralCache
 */

#ifndef ZMALLOC_THREAD_CACHE_H_
#define ZMALLOC_THREAD_CACHE_H_

#include "common.h"

namespace zmalloc {

/**
 * @class FreeList
 * @brief 每个 SizeClass 的空闲对象链表
 */
class FreeList {
public:
  FreeList() = default;

  /// 压入对象
  void Push(void *obj);

  /// 批量压入对象
  void PushRange(void *start, void *end, size_t count);

  /// 弹出对象
  void *Pop();

  /// 批量弹出对象
  void PopRange(void **start, void **end, size_t count);

  /// 链表是否为空
  bool IsEmpty() const { return head_ == nullptr; }

  /// 当前对象数量
  size_t Size() const { return size_; }

  /// 获取最大缓存数量
  size_t MaxSize() const { return max_size_; }

  /// 设置最大缓存数量
  void SetMaxSize(size_t max_size) { max_size_ = max_size; }

private:
  void *head_ = nullptr; ///< 链表头
  size_t size_ = 0;      ///< 当前数量
  size_t max_size_ = 1;  ///< 最大缓存数量（动态调整）
};

/**
 * @class ThreadCache
 * @brief 线程本地缓存
 *
 * 使用 thread_local 保证每个线程有独立的实例。
 */
class ThreadCache {
public:
  /// 获取当前线程的 ThreadCache
  static ThreadCache *GetInstance();

  /**
   * @brief 分配内存
   * @param size 请求大小
   * @return 分配的内存指针，失败返回 nullptr
   */
  void *Allocate(size_t size);

  /**
   * @brief 释放内存
   * @param ptr 内存指针
   * @param size 对象大小
   */
  void Deallocate(void *ptr, size_t size);

private:
  ThreadCache() = default;
  ~ThreadCache();

  /// 从 CentralCache 获取对象
  void *FetchFromCentralCache(size_t class_index, size_t size);

  /// 归还对象给 CentralCache
  void ReleaseToCentralCache(FreeList &list, size_t class_index);

  /// 每个 SizeClass 的空闲链表
  FreeList free_lists_[kNumSizeClasses];

  /// 当前缓存总大小
  size_t cache_size_ = 0;
};

} // namespace zmalloc

#endif // ZMALLOC_THREAD_CACHE_H_
