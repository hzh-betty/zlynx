/**
 * @file free_list.cc
 * @brief FreeList 成员函数实现
 */

#include "zmalloc/internal/free_list.h"

#include <cassert>

#include "zmalloc/internal/prefetch.h"

namespace zmalloc {

void FreeList::push(void *obj) {
    assert(obj); // GCOVR_EXCL_LINE
    // 头插法 O(1) 入链，适合高频小对象释放场景。
    next_obj(obj) = free_list_;
    free_list_ = obj;
    ++size_;
}

void *FreeList::pop() {
    assert(free_list_); // GCOVR_EXCL_LINE
    void *obj = free_list_;
    void *next = next_obj(free_list_);
    free_list_ = next;
    --size_;
    // 预取下一节点，降低后续连续 pop 时的缓存未命中概率。
    prefetch_next(next);
    return obj;
}

void FreeList::push_range(void *start, void *end, size_t n) {
    assert(start && end); // GCOVR_EXCL_LINE
    // 批量对象已在上层串成链，这里只做一次头拼接。
    next_obj(end) = free_list_;
    free_list_ = start;
    size_ += n;
}

void FreeList::pop_range(void *&start, void *&end, size_t n) {
    if (n == 0) {
        start = nullptr;
        end = nullptr;
        return;
    }
    assert(n <= size_); // GCOVR_EXCL_LINE
    start = free_list_;
    end = start;
    // 顺着单链走到第 n 个节点，拆出一段连续子链返回调用方。
    for (size_t i = 0; i < n - 1; ++i) {
        end = next_obj(end);
    }
    free_list_ = next_obj(end);
    next_obj(end) = nullptr;
    size_ -= n;
}

size_t FreeList::pop_batch(void **batch, size_t n) {
    assert(batch); // GCOVR_EXCL_LINE
    if (n == 0) {
        return 0;
    }
    assert(n <= size_); // GCOVR_EXCL_LINE
    void *cur = free_list_;
    for (size_t i = 0; i < n; ++i) {
        batch[i] = cur;
        void *next = next_obj(cur);
        if (next != nullptr) {
            // 批量弹出时显式预取后继，平滑 tight loop 的访问延迟。
            __builtin_prefetch(next, 0, 3);
        }
        cur = next;
    }
    free_list_ = cur;
    next_obj(batch[n - 1]) = nullptr;
    size_ -= n;
    return n;
}

bool FreeList::empty() const { return free_list_ == nullptr; }
size_t FreeList::size() const { return size_; }
size_t &FreeList::max_size() { return max_size_; }

} // namespace zmalloc
