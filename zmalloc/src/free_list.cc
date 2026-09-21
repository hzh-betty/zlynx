/**
 * @file free_list.cc
 * @brief FreeList 成员函数实现
 * @author hzh-betty
 */

#include "zmalloc/internal/free_list.h"

#include <algorithm>
#include <cassert>

#include "zmalloc/internal/prefetch.h"

namespace zmalloc {

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
    low_water_ = std::min(low_water_, size_);
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
        // 批量弹出时预取后继，平滑 tight loop 的访问延迟。
        prefetch_next(next);
        cur = next;
    }
    free_list_ = cur;
    next_obj(batch[n - 1]) = nullptr;
    size_ -= n;
    low_water_ = std::min(low_water_, size_);
    return n;
}

} // namespace zmalloc
