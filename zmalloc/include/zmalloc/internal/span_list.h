#ifndef ZMALLOC_INTERNAL_SPAN_LIST_H_
#define ZMALLOC_INTERNAL_SPAN_LIST_H_

#include <cstddef>

#include "zmalloc_config.h"

namespace zmalloc {

template <typename T> class ObjectPool;

/**
 * @brief Span 元数据，描述一段连续页和其切分状态
 *
 * Span 本身不拥有物理内存，只描述 page cache 里的一段区间：
 * - 大对象场景：obj_size > MAX_BYTES，整段 span 通常只服务一个分配。
 * - 小对象场景：obj_size <= MAX_BYTES，span 会被切成多个固定大小对象，
 *   free_list/use_count 用于追踪对象分配与回收进度。
 */
struct Span {
    PageId page_id = 0; // 首页页号（起始地址 >> PAGE_SHIFT）。
    size_t n = 0;       // 连续页数。

    // SpanList 的双向循环链指针。
    Span *next = nullptr;
    Span *prev = nullptr;

    size_t obj_size = 0;  // 当前切分后的对象大小（字节）。
    size_t use_count = 0; // 已分配出去但尚未归还的对象个数。
    void *free_list = nullptr; // span 内部空闲对象链表头。

    bool is_use = false; // 是否处于活跃分配状态。
};

/**
 * @brief Span 双向循环链表（带哨兵节点）
 *
 * 链表节点由 ObjectPool<Span> 提供，链表本身只负责链接关系。
 * 使用循环哨兵可把空表和非空表操作统一成 O(1) 指针拼接。
 */
class SpanList {
  public:
    SpanList();

    Span *begin() { return head_->next; }
    Span *end() { return head_; }
    bool empty() const { return head_ == head_->next; }

    void push_front(Span *span);
    Span *pop_front();
    void insert(Span *pos, Span *new_span);
    void erase(Span *pos);

  private:
    Span *head_;

    // Span 元数据对象池，降低高频拆分/合并时的 new/delete 抖动。
    static ObjectPool<Span> &span_pool();
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_SPAN_LIST_H_
