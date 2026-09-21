/**
 * @file thread_cache.cc
 * @brief ThreadCache 实现
 * @author hzh-betty

 */

#include "zmalloc/internal/thread_cache.h"

#include <algorithm>
#include <type_traits>

#include "zmalloc/internal/central_cache.h"
#include "zmalloc/internal/transfer_cache.h"

namespace zmalloc {
constexpr size_t ThreadCache::kCacheBudget;
namespace {
// 缓存本体不注册析构；退出钩子关闭后，晚于它执行的 TLS 析构仍可访问本体。
static_assert(std::is_trivially_destructible<ThreadCache>::value,
              "TLS cache must outlive its cleanup hook");
#if defined(__GNUC__) || defined(__clang__)
thread_local ThreadCache tls_thread_cache
    __attribute__((tls_model("initial-exec")));
#else
thread_local ThreadCache tls_thread_cache;
#endif
thread_local bool cleanup_registered = false;

struct ThreadCacheCleanup {
    ~ThreadCacheCleanup() { tls_thread_cache.shutdown(); }
};

constexpr size_t kMaxBatch = 128;
constexpr unsigned kMaxOverages = 3;
} // namespace

ThreadCache *get_thread_cache() {
    if (ZM_UNLIKELY(!cleanup_registered)) {
        // 注册 TLS 析构可能调用 malloc；重入时不得再次注册同一个钩子。
        cleanup_registered = true;
        thread_local ThreadCacheCleanup cleanup;
        (void)cleanup;
    }
    return &tls_thread_cache;
}

void *ThreadCache::fetch_from_central_cache(const SizeClassLookup &e) {
    FreeList &list = free_lists_[e.index];
    // 第一步：根据该大小类的建议批量和动态阈值，决定本次补货数量。
    const size_t batch = std::min<size_t>(e.num_move, kMaxBatch);
    const size_t count = closed_ ? 1 : std::min(list.max_size(), batch);
    void *start = nullptr;
    void *end = nullptr;
    void *objects[kMaxBatch];
    // 第二步：优先从传输缓存接收其他线程归还的对象，避免访问 Span 元数据。
    size_t got = closed_ ? 0
                         : TransferCache::get_instance().remove_range(
                               e.index, objects, count);
    if (got != 0) {
        start = objects[0];
        end = objects[got - 1];
        for (size_t i = 1; i < got; ++i) {
            next_obj(objects[i - 1]) = objects[i];
        }
        next_obj(end) = nullptr;
    } else {
        // 传输缓存未命中，再由中心缓存从 Span 的自由链表补货。
        got = CentralCache::get_instance().fetch_range_obj(
            start, end, count, e.align_size, e.index);
    }
    assert(got > 0 && got <= count);
    if (closed_) {
        return start;
    }
    // 第三步：一个对象立即返回，其余对象挂入当前线程的自由链表。
    class_sizes_[e.index] = e.align_size;
    if (got > 1) {
        list.push_range(next_obj(start), end, got - 1);
        cached_bytes_ += (got - 1) * e.align_size;
    }
    // 先逐个增长到一批，再按批增长；每个规格最多缓存四批。
    if (list.max_size() < batch) {
        ++list.max_size();
    } else {
        list.max_size() = std::min(list.max_size() + batch, 4 * batch);
    }
    // 分配补货不回收其他规格刚预取的对象；预算回收由释放慢路径触发。
    return start;
}

void ThreadCache::release_direct(void *ptr, const SizeClassLookup &e) {
    next_obj(ptr) = nullptr;
    CentralCache::get_instance().release_list_to_spans(ptr, e.align_size,
                                                       e.index);
}

void ThreadCache::release_batch(size_t index, size_t count, bool use_transfer) {
    FreeList &list = free_lists_[index];
    count = std::min(count, list.size());
    const size_t size = class_sizes_[index];
    while (count > 0) {
        // 第一步：使用固定大小的栈上数组从本地自由链表摘出一批指针。
        const size_t n = std::min(count, kMaxBatch);
        void *objects[kMaxBatch];
        list.pop_batch(objects, n);
        cached_bytes_ -= n * size;
        // 第二步：正常回收时先放入传输缓存，线程退出清理时直接跳过该层。
        const size_t inserted =
            use_transfer
                ? TransferCache::get_instance().insert_range(index, objects, n)
                : 0;
        // 第三步：传输缓存放不下的对象重新串链并归还中心缓存。
        // 已插入的指针可能立即被其他线程取走，之后只访问剩余对象。
        if (inserted < n) {
            for (size_t i = inserted + 1; i < n; ++i) {
                next_obj(objects[i - 1]) = objects[i];
            }
            next_obj(objects[n - 1]) = nullptr;
            CentralCache::get_instance().release_list_to_spans(
                objects[inserted], size, index);
        }
        count -= n;
    }
}

void ThreadCache::deallocate_slow(const SizeClassLookup &e) {
    FreeList &list = free_lists_[e.index];
    const size_t batch = std::min<size_t>(e.num_move, kMaxBatch);
    if (list.size() > list.max_size()) {
        // 单个大小类超过阈值时归还一批，并根据历史使用情况调整阈值。
        release_batch(e.index, batch);
        if (list.max_size() < batch) {
            ++list.max_size();
        } else if (list.max_size() > batch &&
                   ++length_overages_[e.index] > kMaxOverages) {
            list.max_size() -= batch;
            length_overages_[e.index] = 0;
        }
    }
    if (cached_bytes_ > kCacheBudget) {
        // 总缓存超过软预算时，再扫描所有大小类回收长期闲置对象。
        scavenge();
    }
}

void ThreadCache::scavenge() {
    for (size_t i = 0; i < NFREELISTS; ++i) {
        FreeList &list = free_lists_[i];
        const size_t low = list.low_water();
        if (low > 0) {
            const size_t batch = std::min<size_t>(
                SizeClass::lookup(class_sizes_[i]).num_move, kMaxBatch);
            // 尽量归还一批，避免扩大批量后低水位回收仍反复搬运零散对象。
            // 不超过低水位，保留本轮实际使用过的对象。
            release_batch(i, std::min(low, std::max(batch, low / 2)));
            if (list.max_size() > batch) {
                list.max_size() = std::max(batch, list.max_size() - batch);
            }
        }
        list.reset_low_water();
    }
}

void ThreadCache::cleanup() {
    for (size_t i = 0; i < NFREELISTS; ++i) {
        release_batch(i, free_lists_[i].size(), false);
        free_lists_[i].max_size() = 1;
        free_lists_[i].reset_low_water();
        length_overages_[i] = 0;
    }
    assert(cached_bytes_ == 0);
}

void ThreadCache::shutdown() {
    closed_ = true;
    cleanup();
}

} // namespace zmalloc
