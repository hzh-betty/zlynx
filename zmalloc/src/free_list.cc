/**
 * @file free_list.cc
 * @brief FreeList 成员函数实现
 */

#include <cassert>

#include "free_list.h"
#include "prefetch.h"

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
  void *next = next_obj(free_list_);
  free_list_ = next;
  --size_;
  prefetch_next(next);
  return obj;
}

void FreeList::push_range(void *start, void *end, size_t n) {
  assert(start && end);
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

size_t FreeList::pop_batch(void **batch, size_t n) {
  assert(batch);
  if (n == 0) {
    return 0;
  }
  assert(n <= size_);
  void *cur = free_list_;
  for (size_t i = 0; i < n; ++i) {
    batch[i] = cur;
    void *next = next_obj(cur);
    if (next != nullptr) {
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
