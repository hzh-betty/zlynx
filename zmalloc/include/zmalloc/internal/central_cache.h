/**
 * @file central_cache.h
 * @brief 中心缓存，跨线程共享，使用桶锁同步
 *
 * @author hzh-betty
 */
#ifndef ZMALLOC_INTERNAL_CENTRAL_CACHE_H_
#define ZMALLOC_INTERNAL_CENTRAL_CACHE_H_

#include <array>
#include <mutex>
#include <new>

#include "common.h"
#include "span_list.h"
#include "zmalloc_config.h"

namespace zmalloc {

// 前向声明
class PageCache;

/**
 * @brief 中心缓存（单例）
 *
 * CentralCache 连接线程级缓存与页级缓存。它从 PageCache 取得 Span，
 * 将 Span 切成固定大小的对象，再批量提供给 ThreadCache。对象全部
 * 归还后，空 Span 会交回 PageCache。
 *
 * 每个大小类维护可分配和已耗尽两条 Span 链表，并使用独立锁。
 * 不同大小类可以并发操作。申请新 Span 时会先释放桶锁，避免持锁
 * 等待耗时较长的页分配。
 */
class CentralCache : public NonCopyable {
  public:
    /**
     * @brief 获取单例实例
     */
    static CentralCache &get_instance() {
        // 全局 malloc/free 在静态析构期间仍可能调用；底层缓存保留到进程结束。
        alignas(
            CentralCache) static unsigned char storage[sizeof(CentralCache)];
        static CentralCache *instance = new (storage) CentralCache;
        return *instance;
    }

    /**
     * @brief 从指定大小类取得一批空闲对象，以对象首字串成链表。
     *
     * @param[out] start 返回链表头；n 为 0 时置空。
     * @param[out] end 返回链表尾，其 next 指针为空；n 为 0 时置空。
     * @param n 请求数量，允许返回不足一批。
     * @param size 小对象字节数，内部按大小类向上对齐。
     * @param index 须与 size 对应的有效大小类索引。
     * @return 实际取得数量；非零请求在正常返回时至少取得一个对象。
     */
    size_t fetch_range_obj(void *&start, void *&end, size_t n, size_t size,
                           size_t index);

    /**
     * @brief 按大小类维护的 Span 双链表及其同步锁。
     *
     * - 每个大小类维护两条 SpanList：
     * - nonempty：至少还有一个可分配对象（span->free_list != nullptr）
     * - empty：已无可分配对象（span->free_list ==
     * nullptr），但仍有对象在外部(ThreadCache)持有
     * - 这样 fetch 不需要线性扫描，通常 O(1) 取到可用 span。
     * - 同一大小类的 nonempty/empty 由同一把锁保护。
     */
    struct alignas(CACHE_LINE_SIZE) CentralFreeList {
        SpanList nonempty;
        SpanList empty;

        // 保护 nonempty/empty 以及 span 在两者之间的迁移。
        SpinLock lock;
    };

    /**
     * @brief 从列表取得或创建一个可分配 Span
     * @param free_list
     * 当前大小类的两条 Span 链表
     * @param size 对象大小（字节）
     *
     * @param lock 已持有的桶锁，必要时会临时释放并重新获取
     * @return
     * 可用于分配对象的 Span
     */
    Span *get_one_span(CentralFreeList &free_list, size_t size,
                       std::unique_lock<SpinLock> &lock);

    /**
     * @brief 按所属 Span 批量归还对象，全部回收的 Span 转交页缓存。
     *
     * @param start 以 next_obj 串联并以空指针结束的对象链；允许为空。
     * @param index 链中全部对象所属的同一大小类索引。
     * @pre 对象尚未归还，调用方须独占此链；返回后不得继续访问链中的对象。
     */
    void release_list_to_spans(void *start, size_t index);

  private:
    friend struct CacheTestAccess;
    CentralCache() = default;

  private:
    std::array<CentralFreeList, NFREELISTS> free_lists_;
};

} // namespace zmalloc

#endif
