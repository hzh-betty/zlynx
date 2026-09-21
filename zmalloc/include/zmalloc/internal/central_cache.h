/**
 * @file central_cache.h
 * @brief 中心缓存，跨线程共享，使用桶锁同步
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_CENTRAL_CACHE_H_
#define ZMALLOC_INTERNAL_CENTRAL_CACHE_H_

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
        alignas(CentralCache) static unsigned char storage[sizeof(CentralCache)];
        static CentralCache *instance = new (storage) CentralCache;
        return *instance;
    }

    /**
     * @brief 从中心缓存获取一批对象，并以单链表形式返回
     * @param start 输出链表头
     * @param end 输出链表尾
     * @param n 期望对象数
     * @param size 对象大小（字节）
     * @return 实际取得的对象数
     */
    size_t fetch_range_obj(void *&start, void *&end, size_t n, size_t size);

    /**
     * @brief 与 fetch_range_obj 相同，但调用方已计算好 size class 索引
     * @note 这是 ThreadCache 热路径的优化重载，避免重复的 size->index 映射。
     */
    size_t fetch_range_obj(void *&start, void *&end, size_t n, size_t size,
                           size_t index);

    /**
     * @brief 按大小类维护的 Span 双链表及其同步锁。
     *
     * - 每个 sizeclass 维护两条 SpanList：
     *   - nonempty：至少还有一个可分配对象（span->free_list != nullptr）
     *   - empty：已无可分配对象（span->free_list ==
     * nullptr），但仍有对象在外部(ThreadCache)持有
     * - 这样 fetch 不需要线性扫描，通常 O(1) 取到可用 span。
     * - 同一 sizeclass 的 nonempty/empty 由同一把锁保护。
     */
    struct alignas(64) CentralFreeList {
        SpanList nonempty;
        SpanList empty;
        // 保护 nonempty/empty 以及 span 在两者之间的迁移。
        SpinLock lock;
    };

    /**
     * @brief 从列表取得或创建一个可分配 Span
     * @param free_list 当前大小类的两条 Span 链表
     * @param size 对象大小（字节）
     * @param lock 已持有的桶锁，必要时会临时释放并重新获取
     * @return 可用于分配对象的 Span
     */
    Span *get_one_span(CentralFreeList &free_list, size_t size,
                       std::unique_lock<SpinLock> &lock);

    /**
     * @brief 将对象链表归还给对应的 Span
     * @param start 链表起始
     * @param size 对象大小
     */
    void release_list_to_spans(void *start, size_t size);

    /**
     * @brief 将对象链归还到已知大小类的 Span
     * @param start 对象链表头
     * @param size 对象大小（字节）
     * @param index 已计算出的大小类索引
     */
    void release_list_to_spans(void *start, size_t size, size_t index);

  private:
    CentralCache() = default;

  private:
    CentralFreeList free_lists_[NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_CENTRAL_CACHE_H_
