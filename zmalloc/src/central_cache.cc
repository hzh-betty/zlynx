/**
 * @file central_cache.cc
 * @brief CentralCache 实现
 * @author hzh-betty

 */
#include "zmalloc/internal/central_cache.h"

#include <cassert>

#include "zmalloc/internal/free_list.h"
#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/size_class.h"
#include "zmalloc/internal/zmalloc_config.h"

namespace zmalloc {

namespace {

#ifndef NDEBUG
bool span_freelist_contains(Span *span, void *target) {
    for (void *it = span->free_list; it != nullptr; it = next_obj(it)) {
        if (it == target) {
            return true;
        }
    }
    return false;
}
#endif

} // namespace

size_t CentralCache::fetch_range_obj(void *&start, void *&end, size_t n,
                                     size_t size) {
    const size_t index = SizeClass::index_fast(size);
    return fetch_range_obj(start, end, n, size, index);
}

size_t CentralCache::fetch_range_obj(void *&start, void *&end, size_t n,
                                     size_t size, size_t index) {
    // 第一步：规范化请求并锁定对应大小类；不同大小类互不阻塞。
    start = end = nullptr;
    if (n == 0) {
        return 0;
    }
    size = SizeClass::round_up_fast(size);
    CentralFreeList &free_list = free_lists_[index];
    std::unique_lock<SpinLock> lock(free_list.lock);
    size_t total = 0;
    while (total < n) {
        // 已有对象时允许返回不足一批，避免为凑批反复申请新 span。
        if (total != 0 && free_list.nonempty.empty()) {
            break;
        }
        Span *span = get_one_span(free_list, size, lock);
        // 第二步：从 Span 的自由链表头摘取本次所需的对象段。
        void *head = span->free_list;
        void *tail = head;
        size_t count = 1;
        while (count < n - total && next_obj(tail) != nullptr) {
            tail = next_obj(tail);
            ++count;
        }
        span->free_list = next_obj(tail);
        next_obj(tail) = nullptr;
        span->use_count += count;
        // 第三步：把多个 Span 提供的对象段拼成一条链返回调用方。
        if (end != nullptr) {
            next_obj(end) = head;
        } else {
            start = head;
        }
        end = tail;
        total += count;
        if (span->free_list == nullptr) {
            // Span 已无可分配对象，移到 empty 链表等待对象归还。
            free_list.nonempty.erase(span);
            free_list.empty.push_front(span);
        }
    }
    return total;
}

Span *CentralCache::get_one_span(CentralFreeList &free_list, size_t size,
                                 std::unique_lock<SpinLock> &lock) {
    // 快路径：直接复用已有可分配 Span。
    if (!free_list.nonempty.empty()) {
        return free_list.nonempty.begin();
    }

    // 页锁等待、系统分配和首次触页不占用桶锁。允许并发补货；
    // span 完成初始化后才发布，异常时两个锁均由 RAII 释放。
    lock.unlock();
    PageCache &pc = PageCache::get_instance();
    Span *span;
    {
        std::lock_guard<std::mutex> page_lock(pc.page_mtx());
        span = pc.new_span(SizeClass::lookup(size).num_pages);
        span->is_use = true;
        span->obj_size = size;
    }
    // 将连续页按固定对象大小原地串成 intrusive 自由链表。
    char *head = reinterpret_cast<char *>(span->page_id << PAGE_SHIFT);
    const size_t count = (span->n << PAGE_SHIFT) / size;
    span->free_list = head;
    for (size_t i = 1; i < count; ++i) {
        next_obj(head) = head + size;
        head += size;
    }
    next_obj(head) = nullptr;
    // 完成初始化后重新获取桶锁，使其他线程只能看到完整的 Span 状态。
    lock.lock();
    free_list.nonempty.push_front(span);
    return span;
}

void CentralCache::release_list_to_spans(void *start, size_t size) {
    const size_t index = SizeClass::index_fast(size);
    release_list_to_spans(start, size, index);
}

void CentralCache::release_list_to_spans(void *start, size_t size,
                                         size_t index) {
    (void)size;
    CentralFreeList &free_list = free_lists_[index];
    if (start == nullptr) {
        return;
    }

    while (start != nullptr) {
        // 两阶段批处理：减少桶锁持有时间。
        // 1) 无锁阶段：把对象按 Span 分组 + 本地拼接，减少 PageMap::get 次数。
        // 2) 持锁阶段：对每个 Span 一次性 splice 链表并批量更新
        // use_count，缩短桶锁持有时间。
        //
        // 任意长度的归还链表按最多 128 个对象分块，限制栈上分组数组。
        Span *spans[128];
        void *group_start[128];
        void *group_end[128];
        size_t group_count[128];
        size_t groups = 0;

        // last_span 小优化：回收链表中相邻对象常来自同一 span。
        // 缓存上一次 span 的页区间，可减少 PageCache::map_object_to_span
        // 调用次数。
        Span *last_span = nullptr;
        PageId last_begin = 0;
        PageId last_end = 0;

        for (size_t count = 0; start != nullptr && count < 128; ++count) {
            void *next = next_obj(start);
            next_obj(start) = nullptr;

            Span *span = nullptr;
            const PageId id = reinterpret_cast<PageId>(start) >> PAGE_SHIFT;
            if (last_span != nullptr && id >= last_begin && id < last_end) {
                span = last_span;
            } else {
                span = PageCache::get_instance().map_object_to_span(start);
                last_span = span;
                last_begin = span->page_id;
                last_end = span->page_id + span->n;
            }

            // 常见情况：回收链表里相邻对象来自同一个 span
            size_t gi = static_cast<size_t>(-1);
            if (groups > 0 && spans[groups - 1] == span) {
                gi = groups - 1;
            } else {
                for (size_t i = 0; i < groups; ++i) {
                    if (spans[i] == span) {
                        gi = i;
                        break;
                    }
                }
            }

            if (gi == static_cast<size_t>(-1)) {
                // new group
                spans[groups] = span;
                group_start[groups] = start;
                group_end[groups] = start;
                group_count[groups] = 1;
                ++groups;
            } else {
                next_obj(group_end[gi]) = start;
                group_end[gi] = start;
                ++group_count[gi];
            }

            start = next;
        }

        // 桶锁内仅更新对象/链表；空 span 摘除后仍标记在用，直到页锁内回收。
        Span *free_spans[128];
        size_t free_count = 0;
        {
            std::lock_guard<SpinLock> lock(free_list.lock);
            for (size_t gi = 0; gi < groups; ++gi) {
                Span *span = spans[gi];
                const bool was_empty = span->free_list == nullptr;
#ifndef NDEBUG
                for (void *it = group_start[gi]; it != nullptr;
                     it = next_obj(it)) {
                    assert(!span_freelist_contains(span, it));
                }
#endif
                next_obj(group_end[gi]) = span->free_list;
                span->free_list = group_start[gi];
                span->use_count -= group_count[gi];
                if (span->use_count == 0) {
                    if (was_empty) {
                        free_list.empty.erase(span);
                    } else {
                        free_list.nonempty.erase(span);
                    }
                    free_spans[free_count++] = span;
                } else if (was_empty) {
                    free_list.empty.erase(span);
                    free_list.nonempty.push_front(span);
                }
            }
        }
        if (free_count != 0) {
            PageCache &pc = PageCache::get_instance();
            std::lock_guard<std::mutex> page_lock(pc.page_mtx());
            for (size_t i = 0; i < free_count; ++i) {
                pc.release_span_to_page_cache(free_spans[i]);
            }
        }
    }
}

} // namespace zmalloc
