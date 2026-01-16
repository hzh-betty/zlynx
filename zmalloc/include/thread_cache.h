#ifndef ZMALLOC_THREAD_CACHE_H_
#define ZMALLOC_THREAD_CACHE_H_

/**
 * @file thread_cache.h
 * @brief 线程本地缓存，每个线程独享，无锁快速分配小对象
 */

#include "common.h"

namespace zmalloc {

// 前向声明
class CentralCache;

/**
 * @brief 线程本地缓存
 *
 * 每个线程独享一个 ThreadCache，用于快速分配和释放小对象。
 * 小于等于 MAX_BYTES 的申请走 ThreadCache，无锁操作。
 */
class ThreadCache {
public:
  /**
   * @brief 分配内存
   * @param size 申请字节数
   * @return 内存指针
   */
  void *allocate(size_t size);

  /**
   * @brief 释放内存
   * @param ptr 内存指针
   * @param size 字节数
   */
  void deallocate(void *ptr, size_t size);

private:
  /**
   * @brief 从 CentralCache 获取对象
   * @param index 哈希桶索引
   * @param size 对象大小
   * @return 对象指针
   */
  void *fetch_from_central_cache(size_t index, size_t size);

  /**
   * @brief 自由链表过长时，回收部分对象到 CentralCache
   * @param list 自由链表
   * @param size 对象大小
   */
  void list_too_long(FreeList &list, size_t size);

private:
  FreeList free_lists_[NFREELISTS]; // 哈希桶
};

/**
 * @brief 获取当前线程的 ThreadCache
 * @return ThreadCache 指针
 */
ThreadCache *get_thread_cache();

} // namespace zmalloc

#endif // ZMALLOC_THREAD_CACHE_H_
