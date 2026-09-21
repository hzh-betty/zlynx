/**
 * @file free_list.h
 * @brief free_list 定义。
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_FREE_LIST_H_
#define ZMALLOC_INTERNAL_FREE_LIST_H_

#include <algorithm>
#include <cassert>
#include <cstddef>

#include "prefetch.h"

namespace zmalloc {

/**
 * @brief 获取自由链表中下一个对象的引用
 * @param ptr 当前对象指针
 * @return 下一个对象指针的引用
 *
 * FreeList 采用 intrusive 单链表：next 指针直接复用对象首地址存储。
 * 这样可避免额外节点分配，代价是对象最小尺寸必须容纳一个指针。
 */
inline void *&next_obj(void *ptr) { return *static_cast<void **>(ptr); }

/**
 * @brief 小对象自由链表
 *
 * 该结构是 thread cache / central cache 的基础容器，
 * 维护对象链、当前长度与动态批量阈值（max_size_）。
 */
class FreeList {
  public:
    /** @brief 将一个空闲对象插入链表头。对象首字用作 next 指针。 */
    void push(void *obj) {
        assert(obj); // GCOVR_EXCL_LINE
        // 头插法 O(1) 入链，适合高频小对象释放场景。
        next_obj(obj) = free_list_;
        free_list_ = obj;
        ++size_;
    }
    /** @brief 移除并返回链表头对象；调用前链表必须非空。 */
    void *pop() {
        assert(free_list_); // GCOVR_EXCL_LINE
        void *obj = free_list_;
        void *next = next_obj(free_list_);
        free_list_ = next;
        --size_;
        low_water_ = std::min(low_water_, size_);
        // 预取下一节点，降低后续连续 pop 时的缓存未命中概率。
        prefetch_next(next);
        return obj;
    }

    /** @brief 将给定的 n 个对象链入表头，start/end 描述输入链段。 */
    void push_range(void *start, void *end, size_t n);
    /** @brief 从表头移出 n 个对象，并通过 start/end 返回链段。 */
    void pop_range(void *&start, void *&end, size_t n);

    /** @brief 最多移出 n 个对象到数组，返回实际移出数量。 */
    size_t pop_batch(void **batch, size_t n);

    /** @brief 判断链表是否没有空闲对象。 */
    bool empty() const { return free_list_ == nullptr; }
    /** @brief 返回当前空闲对象数。 */
    size_t size() const { return size_; }
    /** @brief 返回可调整的批量补货/回收阈值引用。 */
    size_t &max_size() { return max_size_; }
    /** @brief 返回自上次重置以来观察到的最小链表长度。 */
    size_t low_water() const { return low_water_; }
    /** @brief 将低水位重置为当前链表长度。 */
    void reset_low_water() { low_water_ = size_; }

  private:
    void *free_list_ = nullptr; // 链表头。
    size_t size_ = 0;           // 当前空闲对象数量。
    size_t low_water_ = 0;
    size_t max_size_ = 1; // 批量回填/回收的动态阈值。
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_FREE_LIST_H_
