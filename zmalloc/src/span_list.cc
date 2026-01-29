/**
 * @file span_list.cc
 * @brief SpanList 成员函数实现
 */

#include <cassert>

#include "span_list.h"
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

void SpanList::push_front(Span *span) { insert(begin(), span); }

Span *SpanList::pop_front() {
  Span *front = head_->next;
  erase(front);
  return front;
}

void SpanList::insert(Span *pos, Span *new_span) {
  assert(pos && new_span);
  Span *prev = pos->prev;
  prev->next = new_span;
  new_span->prev = prev;
  new_span->next = pos;
  pos->prev = new_span;
}

void SpanList::erase(Span *pos) {
  assert(pos && pos != head_);
  Span *prev = pos->prev;
  Span *next = pos->next;
  prev->next = next;
  next->prev = prev;
}

} // namespace zmalloc
