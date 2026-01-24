#include "allocator.h"

#include <cstdlib>

namespace zhttp {

void *Allocator::allocate(size_t size) {
  if (size == 0) {
    return nullptr;
  }
  return malloc(size);
}

void *Allocator::reallocate(void *ptr, size_t /*old_size*/, size_t new_size) {
  if (new_size == 0) {
    free(ptr);
    return nullptr;
  }
  return std::realloc(ptr, new_size);
}

void Allocator::deallocate(void *ptr, size_t /*size*/) {
  if (ptr != nullptr) {
    free(ptr);
  }
}

} // namespace zhttp
