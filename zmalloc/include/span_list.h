#ifndef ZMALLOC_SPAN_LIST_H_
#define ZMALLOC_SPAN_LIST_H_

#include <cstddef>

#include "zmalloc_config.h"

namespace zmalloc {

template <typename T> class ObjectPool;

/**
 * @brief Span 结构，管理以页为单位的大块内存
 */
struct Span {
  PageId page_id = 0; // 页号 --- 地址 >> PAGE_SHIFT
  size_t n = 0;       // 页数

  Span *next = nullptr;
  Span *prev = nullptr;

  size_t obj_size = 0; // 对象大小（字节）
  size_t use_count = 0; // 已分配对象数
  void *free_list = nullptr; // 自由链表头指针

  bool is_use = false; // 是否正在使用中
};

/**
 * @brief Span 双向循环链表
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
  static ObjectPool<Span> &span_pool();
};

} // namespace zmalloc

#endif // ZMALLOC_SPAN_LIST_H_
