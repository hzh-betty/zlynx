/**
 * @file page_cache.cc
 * @brief PageCache 实现
 * @author hzh-betty
 */

#include "zmalloc/internal/page_cache.h"

#include "zmalloc/internal/system_alloc.h"

#include <cerrno>

namespace zmalloc {

namespace {

void clear_span_mapping(PageMap &id_span_map, Span *span) {
    if (span == nullptr || span->n == 0) {
        return;
    }
    id_span_map.clear_range(span->page_id, span->n);
}

} // namespace

Span *PageCache::new_span(size_t k) {
    // 异常运行库分配异常对象时也可能调用 malloc，不能重入已持有的页锁。
    internal::AllocatorCallGuard guard;
    assert(k > 0);

    // 第一步：大请求优先精确尺寸复用，未命中再向系统申请，不切分或合并。
    // 关键策略：
    // - 小于等于 (NPAGES-1) 的 span：在 PageCache
    // 内按页数分桶管理，可切分/合并。
    // - 更大的 span：使用独立有界缓存；超过单块上限仍直接释放系统映射。
    if (k > NPAGES - 1) {
        for (Span *span = large_spans_.begin(); span != large_spans_.end();
             span = span->next) {
            if (span->n == k) {
                // 大块可能驱逐并 munmap，始终使用无标记映射和有锁识别。
                id_span_map_.set(span->page_id, span);
                advance_release_cursor(span);
                large_spans_.erase(span);
                if (!span->is_released) {
                    unreleased_free_bytes_ -= k * PAGE_SIZE;
                }
                large_cached_bytes_ -= k * PAGE_SIZE;
                span->is_use = true;
                span->is_released = false;
                span->obj_size = 0;
                span->use_count = 0;
                span->free_list = nullptr;
                return span;
            }
        }
        void *ptr = system_alloc(k);
        Span *span = nullptr;
        try {
            span = span_pool_.allocate();
            span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
            span->n = k;
            span->is_use = true;

            // 普通大对象只登记起始页；override 可为对齐块补登记附加页。
            id_span_map_.set(span->page_id, span);
            mapped_bytes_ += k * PAGE_SIZE;
            return span;
        } catch (...) {
            if (span != nullptr) {
                span_pool_.deallocate(span);
            }
            system_free(ptr, k);
            throw;
        }
    }

    // 第二步：优先从精确桶（k 页桶）直接取，避免切分。
    if (!span_lists_[k].empty()) {
        Span *span = span_lists_[k].begin();
        if (!id_span_map_.ensure(span->page_id, span->n)) {
            throw std::bad_alloc();
        }
        advance_release_cursor(span);
        Span *k_span = span_lists_[k].pop_front();
        if (!k_span->is_released) {
            unreleased_free_bytes_ -= k * PAGE_SIZE;
        }

        // 该 Span 被分配出去，标记为在用。
        k_span->is_use = true;
        k_span->is_released = false;
        k_span->obj_size = 0;
        k_span->use_count = 0;
        k_span->free_list = nullptr;
        k_span->next = nullptr;
        k_span->prev = nullptr;

        // 关键步骤：小对象 span 需要为每一页建立映射，支持：
        // - map_object_to_span（任意对象地址 -> 页号 -> span）
        // - Central/Thread 回收时按对象地址找到 span
        map_cached_span(k_span);
        return k_span;
    }

    // 第三步：精确桶为空时向上找更大的桶，切分出一个 k 页 Span。
    // 切分规则：从大 span 的“头部”切出 k 页，剩余部分回挂到对应桶。
    for (size_t i = k + 1; i < NPAGES; ++i) {
        if (!span_lists_[i].empty()) {
            Span *n_span = span_lists_[i].begin();
            if (!id_span_map_.ensure(n_span->page_id, n_span->n)) {
                throw std::bad_alloc();
            }
            Span *k_span = span_pool_.allocate();
            advance_release_cursor(n_span);
            span_lists_[i].pop_front();
            if (!n_span->is_released) {
                unreleased_free_bytes_ -= k * PAGE_SIZE;
            }

            // 在 n_span 头部切 k 页
            k_span->page_id = n_span->page_id;
            k_span->n = k;
            k_span->is_use = true;
            k_span->obj_size = 0;
            k_span->use_count = 0;
            k_span->free_list = nullptr;
            k_span->next = nullptr;
            k_span->prev = nullptr;

            n_span->page_id += k;
            n_span->n -= k;
            n_span->is_use = false;
            n_span->obj_size = 0;
            n_span->use_count = 0;
            n_span->free_list = nullptr;
            n_span->next = nullptr;
            n_span->prev = nullptr;

            // 剩余部分挂到对应桶
            span_lists_[n_span->n].push_front(n_span);

            // 关键步骤：为“剩余 span”建立首尾页映射，用于后续合并。
            // 注意：为了节省空间，只在空闲 span
            // 上维护首尾页映射即可完成合并判断。
            id_span_map_.set(n_span->page_id, n_span);
            id_span_map_.set(n_span->page_id + n_span->n - 1, n_span);

            // 建立 k_span 所有页的映射
            map_cached_span(k_span);
            return k_span;
        }
    }

    // 第四步：没有可切分 Span 时向系统补充最大缓存块，再重新执行查找。
    // 向系统申请 (NPAGES-1) 页作为“补货”，挂入最大桶。
    // 然后递归再走一次 new_span(k)（此时一定能在向上搜索中命中）。
    Span *big_span = span_pool_.allocate();
    void *ptr = nullptr;
    try {
        ptr = system_alloc(NPAGES - 1);
        const PageId id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
        if (!id_span_map_.ensure(id, NPAGES - 1)) {
            throw std::bad_alloc();
        }
    } catch (...) {
        if (ptr != nullptr) {
            system_free(ptr, NPAGES - 1);
        }
        span_pool_.deallocate(big_span);
        throw;
    }
    big_span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
    big_span->n = NPAGES - 1;
    big_span->is_use = false;
    big_span->obj_size = 0;
    big_span->use_count = 0;
    big_span->free_list = nullptr;
    big_span->next = nullptr;
    big_span->prev = nullptr;

    id_span_map_.set(big_span->page_id, big_span);
    id_span_map_.set(big_span->page_id + big_span->n - 1, big_span);

    span_lists_[big_span->n].push_front(big_span);
    mapped_bytes_ += big_span->n * PAGE_SIZE;
    unreleased_free_bytes_ += big_span->n * PAGE_SIZE;

    // 递归调用
    return new_span(k);
}

void PageCache::release_span_to_page_cache(Span *span) {
    // 第一步：清除旧映射，避免合并过程中查询到已失效的 Span 边界。
    clear_span_mapping(id_span_map_, span);
    const size_t returned_bytes = span->n * PAGE_SIZE;

    // 第二步：大 Span 进入独立缓存，超预算驱逐，超大块直接释放给系统。
    if (span->n > NPAGES - 1) {
        const size_t bytes = span->n * PAGE_SIZE;
        if (span->n <= LARGE_CACHE_MAX_PAGES) {
            // 只缓存有限大小的大块；驱逐最早归还的空闲块，严格遵守总预算。
            while (large_cached_bytes_ > LARGE_CACHE_BUDGET - bytes) {
                Span *old = large_spans_.end()->prev;
                advance_release_cursor(old);
                large_spans_.erase(old);
                if (!old->is_released) {
                    unreleased_free_bytes_ -= old->n * PAGE_SIZE;
                }
                const size_t old_bytes = old->n * PAGE_SIZE;
                system_free(reinterpret_cast<void *>(old->page_id << PAGE_SHIFT),
                            old->n);
                large_cached_bytes_ -= old_bytes;
                mapped_bytes_ -= old_bytes;
                span_pool_.deallocate(old);
            }
            span->is_use = false;
            span->is_released = false;
            span->obj_size = 0;
            span->use_count = 0;
            span->free_list = nullptr;
            large_spans_.push_front(span);
            large_cached_bytes_ += bytes;
            unreleased_free_bytes_ += bytes;
            maybe_release_free_pages(returned_bytes);
            return;
        }
        void *ptr = reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
        system_free(ptr, span->n);
        mapped_bytes_ -= span->n * PAGE_SIZE;
        span_pool_.deallocate(span);
        return;
    }

    unreleased_free_bytes_ += returned_bytes;
    // 第三步：尝试与相邻空闲 Span 合并，减少外碎片。
    // 停止条件：
    // - 相邻 span 不存在
    // - 相邻 span 正在使用（is_use==true）
    // - 合并后超过桶最大管理页数（NPAGES-1）
    // 注意：这里依赖 id_span_map_ 的“边界页映射”来定位相邻 span。
    //
    // 1) 向前合并
    while (true) {
        PageId prev_id = span->page_id - 1;
        Span *ret = decode_span(id_span_map_.get(prev_id));
        if (ret == nullptr) {
            break;
        }
        Span *prev_span = ret;
        if (prev_span->is_use) {
            break;
        }
        if (prev_span->n + span->n > NPAGES - 1) {
            break;
        }

        span->page_id = prev_span->page_id;
        span->n += prev_span->n;

        clear_span_mapping(id_span_map_, prev_span);
        advance_release_cursor(prev_span);
        span_lists_[prev_span->n].erase(prev_span);
        if (prev_span->is_released) {
            unreleased_free_bytes_ += prev_span->n * PAGE_SIZE;
        }
        span_pool_.deallocate(prev_span);
    }

    // 2) 向后合并
    while (true) {
        PageId next_id = span->page_id + span->n;
        Span *ret = decode_span(id_span_map_.get(next_id));
        if (ret == nullptr) {
            break;
        }
        Span *next_span = ret;
        if (next_span->is_use) {
            break;
        }
        if (next_span->n + span->n > NPAGES - 1) {
            break;
        }

        span->n += next_span->n;

        clear_span_mapping(id_span_map_, next_span);
        advance_release_cursor(next_span);
        span_lists_[next_span->n].erase(next_span);
        if (next_span->is_released) {
            unreleased_free_bytes_ += next_span->n * PAGE_SIZE;
        }
        span_pool_.deallocate(next_span);
    }

    // 第四步：按合并后的页数挂回对应桶，并重建首尾页映射。
    span->obj_size = 0;
    span->use_count = 0;
    span->free_list = nullptr;
    span_lists_[span->n].push_front(span);

    // 空闲 span 只维护最终首尾页映射，匹配 tcmalloc 风格的 pagemap 约定。
    id_span_map_.set(span->page_id, span);
    id_span_map_.set(span->page_id + span->n - 1, span);

    span->is_use = false;
    // 新归还的页可能已被触碰；合并后整段重新允许建议回收。
    span->is_released = false;
    maybe_release_free_pages(returned_bytes);
}

void PageCache::maybe_release_free_pages(size_t returned_bytes) {
    if (!AUTO_RELEASE_ENABLED) {
        return;
    }
    // 仅整段页归还走此路径；TLS/传输缓存的逐对象热路径不扫描、不系统调用。
    if (returned_since_auto_release_ < AUTO_RELEASE_INTERVAL_BYTES) {
        returned_since_auto_release_ += returned_bytes;
    }
    if (unreleased_free_bytes_ < AUTO_RELEASE_MIN_FREE_BYTES ||
        returned_since_auto_release_ < AUTO_RELEASE_INTERVAL_BYTES) {
        return;
    }
    returned_since_auto_release_ = 0;
    const int saved_errno = errno;
    size_t bytes = 0;
    size_t scanned = 0;
    size_t buckets = 0;
    // 从上轮停下的位置继续，避免已回收节点反复占用检查预算。
    while (buckets < NPAGES && scanned < AUTO_RELEASE_MAX_SPANS &&
           bytes < AUTO_RELEASE_MAX_BYTES) {
        SpanList &list = auto_release_bucket_ == 0
                             ? large_spans_ : span_lists_[auto_release_bucket_];
        if (auto_release_cursor_ == nullptr) {
            auto_release_cursor_ = list.end()->prev;
        }
        while (auto_release_cursor_ != list.end() &&
               scanned < AUTO_RELEASE_MAX_SPANS && bytes < AUTO_RELEASE_MAX_BYTES) {
            Span *span = auto_release_cursor_;
            auto_release_cursor_ = span->prev;
            ++scanned;
            const size_t span_bytes = span->n * PAGE_SIZE;
            assert(!span->is_use);
            if (!span->is_released && span_bytes <= AUTO_RELEASE_MAX_BYTES - bytes &&
                system_release(reinterpret_cast<void *>(span->page_id << PAGE_SHIFT),
                               span->n)) {
                span->is_released = true;
                bytes += span_bytes;
            }
        }
        if (auto_release_cursor_ != list.end()) {
            break;
        }
        auto_release_cursor_ = nullptr;
        auto_release_bucket_ = (auto_release_bucket_ + 1) % NPAGES;
        ++buckets;
    }
    unreleased_free_bytes_ -= bytes;
    total_released_bytes_ += bytes;
    errno = saved_errno;
}

size_t PageCache::release_free_pages() {
    size_t bytes = 0;
    // 遍历和系统调用均持有页锁，分配路径不能同时取得正在回收的 Span。
    for (size_t i = 1; i < NPAGES; ++i) {
        SpanList &list = span_lists_[i];
        for (Span *span = list.begin(); span != list.end(); span = span->next) {
            assert(!span->is_use);
            if (!span->is_released &&
                system_release(reinterpret_cast<void *>(span->page_id << PAGE_SHIFT),
                               span->n)) {
                span->is_released = true;
                bytes += span->n * PAGE_SIZE;
            }
        }
    }
    for (Span *span = large_spans_.begin(); span != large_spans_.end();
         span = span->next) {
        if (!span->is_released &&
            system_release(reinterpret_cast<void *>(span->page_id << PAGE_SHIFT),
                           span->n)) {
            span->is_released = true;
            bytes += span->n * PAGE_SIZE;
        }
    }
    unreleased_free_bytes_ -= bytes;
    returned_since_auto_release_ = 0;
    total_released_bytes_ += bytes;
    return bytes;
}

PageCacheStats PageCache::statistics() {
    PageCacheStats stats{0, 0, mapped_bytes_, total_released_bytes_};
    for (size_t i = 1; i < NPAGES; ++i) {
        SpanList &list = span_lists_[i];
        for (Span *span = list.begin(); span != list.end(); span = span->next) {
            stats.free_bytes += span->n * PAGE_SIZE;
            if (span->is_released) {
                stats.released_bytes += span->n * PAGE_SIZE;
            }
        }
    }
    stats.free_bytes += large_cached_bytes_;
    for (Span *span = large_spans_.begin(); span != large_spans_.end();
         span = span->next) {
        if (span->is_released) {
            stats.released_bytes += span->n * PAGE_SIZE;
        }
    }
    return stats;
}

} // namespace zmalloc
