/**
 * @file free_list.cc
 * @brief FreeList 成员函数实现
 */

#include "common.h"

namespace zmalloc {

void FreeList::push(void *obj) {
  assert(obj);
  next_obj(obj) = free_list_;
  free_list_ = obj;
  ++size_;
}

void *FreeList::pop() {
  assert(free_list_);
  void *obj = free_list_;
  free_list_ = next_obj(free_list_);
  --size_;
  return obj;
}

void FreeList::push_range(void *start, void *end, size_t n) {
  assert(start && end);
  next_obj(end) = free_list_;
  free_list_ = start;
  size_ += n;
}

void FreeList::pop_range(void *&start, void *&end, size_t n) {
  assert(n <= size_);
  start = free_list_;
  end = start;
  for (size_t i = 0; i < n - 1; ++i) {
    end = next_obj(end);
  }
  free_list_ = next_obj(end);
  next_obj(end) = nullptr;
  size_ -= n;
}

} // namespace zmalloc
