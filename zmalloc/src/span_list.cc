/**
 * @file span_list.cc
 * @brief SpanList 实现（common.h 中声明的非内联函数）
 */

#include "common.h"
#include "object_pool.h"

namespace zmalloc {

ObjectPool<Span> &SpanList::span_pool() {
  static ObjectPool<Span> pool;
  return pool;
}

SpanList::SpanList() {
  head_ = span_pool().allocate();
  head_->next = head_;
  head_->prev = head_;
}

} // namespace zmalloc
