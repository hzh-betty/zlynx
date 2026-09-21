/**
 * @file transfer_cache.h
 * @brief 传输缓存，位于 ThreadCache 和 CentralCache 之间的批量缓存层
 * @author hzh-betty
 *
 * 参考 tcmalloc 的 Transfer Cache 设计：
 * - 使用环形缓冲区存储批量对象指针
 * - 减少 ThreadCache 与 CentralCache 之间的锁竞争
 * - 批量传输提高缓存效率
 */

#ifndef ZMALLOC_INTERNAL_TRANSFER_CACHE_H_
#define ZMALLOC_INTERNAL_TRANSFER_CACHE_H_

#include <atomic>
#include <cassert>
#include <new>

#include "common.h"
#include "zmalloc_config.h"

namespace zmalloc {

/**
 * @brief 传输缓存单个 size class 的缓存
 *
 * 采用环形缓冲区存储对象指针，支持批量插入和批量获取。
 * 每个 size class 对应一个独立的 TransferCacheEntry。
 */
class TransferCacheEntry {
  public:
    // 指针槽位的物理上限；各规格另有按字节预算计算的逻辑容量。
    static constexpr size_t kMaxCacheSlots = 2048;
    static constexpr size_t kMask = kMaxCacheSlots - 1;
    static_assert((kMaxCacheSlots & (kMaxCacheSlots - 1)) == 0,
                  "kMaxCacheSlots must be power of two");

    /** @brief 创建一个容量不超过物理槽位数的环形缓存。 */
    explicit TransferCacheEntry(size_t capacity = kMaxCacheSlots)
        : capacity_(capacity) {
        assert(capacity <= kMaxCacheSlots);
    }

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
    bool full() const { return size() >= capacity_; }

  private:
    friend class TransferCache;
    size_t capacity_;
    mutable SpinLock mtx_;
    void *slots_[kMaxCacheSlots];  // 环形缓冲区
    size_t head_ = 0;              // 插入位置
    size_t tail_ = 0;              // 取出位置
    std::atomic<size_t> count_{0}; // 当前对象数量
};

/**
 * @brief 传输缓存管理器（单例）
 *
 * TransferCache 位于 ThreadCache 与 CentralCache 之间。一个线程
 * 归还的批量对象可以直接被另一线程取得，从而减少访问 Span 链表和
 * PageMap 的次数。
 *
 * 每个大小类使用独立的定长环形缓冲区和锁。容量按对象字节数限制，
 * 避免大对象规格占用过多常驻内存。
 */
class TransferCache : public NonCopyable {
  public:
    /**
     * @brief 获取单例实例
     */
    static TransferCache &get_instance() {
        // 缓存可被晚期 TLS/静态析构访问，生命周期覆盖整个进程。
        alignas(TransferCache) static unsigned char
            storage[sizeof(TransferCache)];
        static TransferCache *instance = new (storage) TransferCache;
        return *instance;
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
    TransferCache();

  private:
    TransferCacheEntry entries_[NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_TRANSFER_CACHE_H_
