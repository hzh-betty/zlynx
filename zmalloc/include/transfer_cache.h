#ifndef ZMALLOC_TRANSFER_CACHE_H_
#define ZMALLOC_TRANSFER_CACHE_H_

/**
 * @file transfer_cache.h
 * @brief 传输缓存，位于 ThreadCache 和 CentralCache 之间的批量缓存层
 *
 * 参考 tcmalloc 的 Transfer Cache 设计：
 * - 使用环形缓冲区存储批量对象指针
 * - 减少 ThreadCache 与 CentralCache 之间的锁竞争
 * - 批量传输提高缓存效率
 */

#include "common.h"
#include "zmalloc_noncopyable.h"

namespace zmalloc {

/**
 * @brief 传输缓存单个 size class 的缓存
 *
 * 采用环形缓冲区存储对象指针，支持批量插入和批量获取。
 * 每个 size class 对应一个独立的 TransferCacheEntry。
 */
class TransferCacheEntry {
public:
  // 环形缓冲区最大容量（64个批次，每批次最多128个对象）
  static constexpr size_t kMaxCacheSlots = 2048;
  static constexpr size_t kMask = kMaxCacheSlots - 1;
  static_assert((kMaxCacheSlots & (kMaxCacheSlots - 1)) == 0,
                "kMaxCacheSlots must be power of two");

  TransferCacheEntry() = default;

  /**
   * @brief 批量插入对象到缓存
   * @param batch 对象指针数组
   * @param count 对象数量
   * @return 实际插入的对象数量
   */
  size_t insert_range(void *batch[], size_t count);

  /**
   * @brief 批量获取对象
   * @param batch 输出对象指针数组
   * @param count 请求的对象数量
   * @return 实际获取的对象数量
   */
  size_t remove_range(void *batch[], size_t count);

  /**
   * @brief 尝试批量插入（无阻塞）
   * @param batch 对象指针数组
   * @param count 对象数量
   * @param inserted 输出实际插入的对象数量
   * @return true: 操作完成（可能插入0个）; false: 锁竞争，需要 fallback
   */
  bool try_insert_range(void *batch[], size_t count, size_t &inserted);

  /**
   * @brief 尝试批量获取（无阻塞）
   * @param batch 输出对象指针数组
   * @param count 请求的对象数量
   * @param removed 输出实际获取的对象数量
   * @return true: 操作完成（可能获取0个）; false: 锁竞争，需要 fallback
   */
  bool try_remove_range(void *batch[], size_t count, size_t &removed);

  /**
   * @brief 获取当前缓存的对象数量
   */
  size_t size() const;

  /**
   * @brief 缓存是否为空
   */
  bool empty() const { return size() == 0; }

  /**
   * @brief 缓存是否已满
   */
  bool full() const { return size() >= kMaxCacheSlots; }

private:
  mutable SpinLock mtx_;
  void *slots_[kMaxCacheSlots]; // 环形缓冲区
  size_t head_ = 0;             // 插入位置
  size_t tail_ = 0;             // 取出位置
  // 当前对象数量：
  // - 允许锁外读取用于快判断（空/满）
  // - 修改仍在锁内完成
  std::atomic<size_t> count_{0};
};

/**
 * @brief 传输缓存管理器（单例）
 *
 * 管理所有 size class 的 TransferCacheEntry。
 */
class TransferCache : public NonCopyable {
public:
  /**
   * @brief 获取单例实例
   */
  static TransferCache &get_instance() {
    static TransferCache instance;
    return instance;
  }

  /**
   * @brief 获取指定 size class 的缓存条目
   * @param index size class 索引
   */
  TransferCacheEntry &get_entry(size_t index) { return entries_[index]; }

  /**
   * @brief 批量插入对象到指定 size class 的缓存
   * @param index size class 索引
   * @param batch 对象指针数组
   * @param count 对象数量
   * @return 实际插入的对象数量
   */
  size_t insert_range(size_t index, void *batch[], size_t count);

  /**
   * @brief 批量获取指定 size class 的对象
   * @param index size class 索引
   * @param batch 输出对象指针数组
   * @param count 请求的对象数量
   * @return 实际获取的对象数量
   */
  size_t remove_range(size_t index, void *batch[], size_t count);

  /**
   * @brief 尝试批量插入（无阻塞）
   * @param index size class 索引
   * @param batch 对象指针数组
   * @param count 对象数量
   * @param inserted 输出实际插入的对象数量
   * @return true: 操作完成; false: 锁竞争
   */
  bool try_insert_range(size_t index, void *batch[], size_t count,
                        size_t &inserted);

  /**
   * @brief 尝试批量获取（无阻塞）
   * @param index size class 索引
   * @param batch 输出对象指针数组
   * @param count 请求的对象数量
   * @param removed 输出实际获取的对象数量
   * @return true: 操作完成; false: 锁竞争
   */
  bool try_remove_range(size_t index, void *batch[], size_t count,
                        size_t &removed);

private:
  TransferCache() = default;

private:
  TransferCacheEntry entries_[NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_TRANSFER_CACHE_H_
