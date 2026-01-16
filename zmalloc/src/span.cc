/**
 * @file span.cc
 * @brief Span 和 SpanList 实现
 */

#include "span.h"

namespace zmalloc {

void *Span::PopFreeObject() {
  if (free_list_ == nullptr) {
    return nullptr;
  }

  void *obj = free_list_;
  // 链表下一个节点存储在对象的前 sizeof(void*) 字节
  free_list_ = *reinterpret_cast<void **>(obj);
  return obj;
}

void Span::PushFreeObject(void *obj) {
  // 将当前 free_list_ 存储到 obj 的前 sizeof(void*) 字节
  *reinterpret_cast<void **>(obj) = free_list_;
  free_list_ = obj;
}

SpanList::SpanList() {
  head_.next = &head_;
  head_.prev = &head_;
}

void SpanList::PushFront(Span *span) {
  span->next = head_.next;
  span->prev = &head_;
  head_.next->prev = span;
  head_.next = span;
}

void SpanList::Erase(Span *span) {
  span->prev->next = span->next;
  span->next->prev = span->prev;
  span->prev = nullptr;
  span->next = nullptr;
}

Span *SpanList::PopFront() {
  if (IsEmpty()) {
    return nullptr;
  }
  Span *span = head_.next;
  Erase(span);
  return span;
}

} // namespace zmalloc
