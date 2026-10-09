/**
 * @file span_list.h
 * @brief span_list 定义。
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_SPAN_LIST_H_
#define ZMALLOC_INTERNAL_SPAN_LIST_H_

#include <cstddef>

#include "zmalloc_config.h"

namespace zmalloc {

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

    size_t obj_size = 0;       // 当前切分后的对象大小（字节）。
    size_t use_count = 0;      // 已分配出去但尚未归还的对象个数。
    void *free_list = nullptr; // span 内部空闲对象链表头。

    bool is_use = false; // 是否处于活跃分配状态。
    bool is_released = false; // 空闲 Span 的整段页是否已成功建议物理回收。
};

/**
 * @brief Span 双向循环链表（带哨兵节点）
 *
 * 哨兵内嵌于链表，普通节点由调用方提供，链表本身只负责链接关系。
 * 使用循环哨兵可把空表和非空表操作统一成 O(1) 指针拼接。
 */
class SpanList {
  public:
    /** @brief 创建只含哨兵节点的空循环链表。 */
    SpanList();

    // 节点保存哨兵地址，复制或移动会破坏循环链的链接关系。
    SpanList(const SpanList &) = delete;
    SpanList &operator=(const SpanList &) = delete;
    SpanList(SpanList &&) = delete;
    SpanList &operator=(SpanList &&) = delete;

    /** @brief 返回首个 Span；空表时返回 end()。 */
    Span *begin() { return head_.next; }
    /** @brief 返回哨兵节点，作为遍历结束标记。 */
    Span *end() { return &head_; }
    /** @brief 判断链表是否为空。 */
    bool empty() const { return &head_ == head_.next; }

    /** @brief 将 Span 插入链表头。 */
    void push_front(Span *span);
    /** @brief 移除并返回链表头 Span；调用前链表必须非空。 */
    Span *pop_front();
    /** @brief 将 new_span 插入到 pos 之前。 */
    void insert(Span *pos, Span *new_span);
    /** @brief 从链表中摘除指定 Span，不释放其元数据。 */
    void erase(Span *pos);

  private:
    // 每个链表独立持有哨兵，避免并发初始化时访问共享的无锁对象池。
    Span head_;
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_SPAN_LIST_H_
