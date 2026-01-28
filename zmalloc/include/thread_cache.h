#ifndef ZMALLOC_THREAD_CACHE_H_
#define ZMALLOC_THREAD_CACHE_H_

/**
 * @file thread_cache.h
 * @brief 线程本地缓存，每个线程独享，无锁快速分配小对象
 */

#include "common.h"
#include "zmalloc_noncopyable.h"

namespace zmalloc {

// 前向声明
class CentralCache;

/**
 * @brief 线程本地缓存
 *
 * 每个线程独享一个 ThreadCache，用于快速分配和释放小对象。
 * 小于等于 MAX_BYTES 的申请走 ThreadCache，无锁操作。
 */
class ThreadCache : public NonCopyable {
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

  /**
   * @brief 快路径弹出（无锁直接弹出，供 zmalloc 热路径调用）
   * @param index 哈希桶索引
   * @return 对象指针，如果链表为空返回 nullptr
   */
  ZM_ALWAYS_INLINE void *try_pop_fast(size_t index) {
    if (ZM_LIKELY(!free_lists_[index].empty())) {
      return free_lists_[index].pop();
    }
    return nullptr;
  }

  /**
   * @brief 快路径压入（无锁直接压入，不检查过长，供 zfree 热路径调用）
   * @param ptr 对象指针
   * @param index 哈希桶索引
   * @return true: 成功且无需回收；false: 链表过长需走慢路径
   */
  ZM_ALWAYS_INLINE bool try_push_fast(void *ptr, size_t index) {
    FreeList &list = free_lists_[index];
    // 先检查：如果链表已经接近满，直接返回 false 让调用方走慢路径
    if (ZM_UNLIKELY(list.size() >= list.max_size())) {
      return false;
    }
    // 快路径：压入对象
    list.push(ptr);
    return true;
  }

private:
  /**
   * @brief 从 CentralCache 获取对象
   * @param index 哈希桶索引
   * @param size 对象大小
   * @return 对象指针
   */
  void *fetch_from_central_cache(size_t index, size_t size);

  /**
   * @brief 从 CentralCache 获取对象（调用方传入批量策略）
   * @param index 哈希桶索引
   * @param size 对象大小
   * @param num_move_size 建议的批量搬运个数（通常来自 SizeClass 查表）
   */
  void *fetch_from_central_cache(size_t index, size_t size, size_t num_move);

  /**
   * @brief 自由链表过长时，回收部分对象到 CentralCache
   * @param list 自由链表
   * @param size 对象大小
   * @param index 哈希桶索引（由调用方预先计算，避免重复查表）
   */
  void list_too_long(FreeList &list, size_t size, size_t index);

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
