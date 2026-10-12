/**
 * @file transfer_cache.h
 * @brief 传输缓存，位于 ThreadCache 和
 * CentralCache 之间的批量缓存层
 * @author hzh-betty
 *
 * 参考 tcmalloc 的
 * Transfer Cache 设计：
 * - 使用环形缓冲区存储批量对象指针
 * - 减少
 * ThreadCache 与 CentralCache 之间的锁竞争
 * - 批量传输提高缓存效率

 */
#ifndef ZMALLOC_INTERNAL_TRANSFER_CACHE_H_
#define ZMALLOC_INTERNAL_TRANSFER_CACHE_H_

#include <array>
#include <atomic>
#include <cassert>
#include <new>

#include "common.h"
#include "zmalloc_config.h"

namespace zmalloc {

/**
 * @brief 一个大小类的共享环形指针缓存，按批转移空闲对象。
 *
 * mtx_ 保护槽位及读写位置；count_ 仅提供无锁数量采样，不发布槽位内容。
 * 插入或取出会重新在锁内确定数量，调用方仍持有未成功转移的对象。
 */
class TransferCacheEntry {
  public:
    // 指针槽位的物理上限；各规格另有按字节预算计算的逻辑容量。
    static constexpr size_t kMaxCacheSlots = TRANSFER_CACHE_SLOTS;
    static constexpr size_t kMask = kMaxCacheSlots - 1;
    static_assert((kMaxCacheSlots & (kMaxCacheSlots - 1)) == 0,
                  "kMaxCacheSlots must be power of two");

    /**
     * @brief 创建一个容量不超过物理槽位数的环形缓存。

     */
    explicit TransferCacheEntry(size_t capacity = kMaxCacheSlots)
        : capacity_(capacity) {
        assert(capacity <= kMaxCacheSlots);
    }

    /**
     * @brief 尝试接管 batch 前缀中的空闲对象；容量不足时只插入部分。
     *
     * @param batch 至少含 count 个有效指针的数组，count 为 0 时不访问。
     * @param count 请求插入数量。
     * @return 实际插入的前缀长度；其余对象仍由调用方持有。
     * @pre
     * 所有对象属于当前大小类；已插入对象可能立即被其他线程取得，不能再访问。
     */
    size_t insert_range(void *batch[], size_t count);

    /**
     * @brief 从缓存取出对象，将其所有权交给调用方。
     *
     * @param[out] batch 可写指针数组，容量至少为 count；只改写实际取出的前缀。
     * @param count 最大取出数量，0 时不访问数组。
     * @return 实际取出数量；缓存为空或被并发取空时可为 0。
     */
    size_t remove_range(void *batch[], size_t count);

    /**
     * @brief 返回缓存对象数的瞬时采样，不保证后续操作仍能取得同样数量。
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

    // 逻辑容量可小于物理槽位数；下标回绕仍按 kMaxCacheSlots 计算。
    size_t capacity_;
    mutable SpinLock mtx_;
    std::array<void *, kMaxCacheSlots> slots_; // 环形缓冲区
    size_t head_ = 0;                          // 插入位置
    size_t tail_ = 0;                          // 取出位置
    std::atomic<size_t> count_{0};             // 当前对象数量
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
        alignas(
            TransferCache) static unsigned char storage[sizeof(TransferCache)];
        static TransferCache *instance = new (storage) TransferCache;
        return *instance;
    }

    /**
     * @brief 获取指定 大小类 的缓存条目
     * @param index 大小类 索引
     */
    TransferCacheEntry &get_entry(size_t index) { return entries_[index]; }

    /**
     * @brief 批量插入对象到指定 大小类 的缓存
     * @param index 大小类 索引
     * @param batch 对象指针数组
     * @param count 对象数量
     * @return 实际插入的对象数量
     */
    size_t insert_range(size_t index, void *batch[], size_t count);

    /**
     * @brief 批量获取指定 大小类 的对象
     * @param index 大小类 索引
     * @param batch 输出对象指针数组
     * @param count 请求的对象数量
     * @return 实际获取的对象数量
     */
    size_t remove_range(size_t index, void *batch[], size_t count);

    /**
     * @brief 按每类初始数量有界归还中心层；返回归还对象的字节数。

     */
    size_t drain();

    /**
     * @brief 各大小类分别取样的缓存字节数，并发下不是整体原子快照。

     */
    size_t cached_bytes() const;

  private:
    TransferCache();

  private:
    std::array<TransferCacheEntry, NFREELISTS> entries_;
};

} // namespace zmalloc

#endif
