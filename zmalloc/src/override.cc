#include "page_cache.h"
#include "system_alloc.h"
#include "zmalloc.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <atomic>

namespace zmalloc {
namespace internal {

namespace {

struct BootstrapAlloc {
  BootstrapAlloc *next;
  void *user_ptr;
  size_t user_size;
  size_t mapping_pages;
};

BootstrapAlloc *&bootstrap_head() {
  static BootstrapAlloc *head = nullptr;
  return head;
}

SpinLock &bootstrap_lock() {
  static SpinLock lock;
  return lock;
}

std::atomic<bool> &allocator_ready() {
  static std::atomic<bool> ready{false};
  return ready;
}

thread_local bool tls_initializing_allocator = false;
thread_local size_t tls_allocator_call_depth = 0;

class AllocatorCallGuard {
public:
  AllocatorCallGuard() noexcept { ++tls_allocator_call_depth; }
  ~AllocatorCallGuard() { --tls_allocator_call_depth; }
};

size_t bootstrap_mapping_pages(size_t size) {
  const size_t total = sizeof(BootstrapAlloc) + size;
  return (total + PAGE_SIZE - 1) >> PAGE_SHIFT;
}

void *bootstrap_allocate(size_t size) noexcept {
  if (size == 0) {
    return nullptr;
  }

  BootstrapAlloc *alloc = static_cast<BootstrapAlloc *>(
      system_alloc(bootstrap_mapping_pages(size)));
  alloc->user_ptr = alloc + 1;
  alloc->user_size = size;
  alloc->mapping_pages = bootstrap_mapping_pages(size);

  SpinLock &lock = bootstrap_lock();
  lock.lock();
  alloc->next = bootstrap_head();
  bootstrap_head() = alloc;
  lock.unlock();
  return alloc->user_ptr;
}

BootstrapAlloc *find_bootstrap_alloc(void *ptr, BootstrapAlloc **prev_out) noexcept {
  BootstrapAlloc *prev = nullptr;
  BootstrapAlloc *cur = bootstrap_head();
  while (cur != nullptr) {
    if (cur->user_ptr == ptr) {
      if (prev_out != nullptr) {
        *prev_out = prev;
      }
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  return nullptr;
}

size_t bootstrap_size(void *ptr) noexcept {
  SpinLock &lock = bootstrap_lock();
  lock.lock();
  BootstrapAlloc *alloc = find_bootstrap_alloc(ptr, nullptr);
  const size_t size = alloc == nullptr ? 0 : alloc->user_size;
  lock.unlock();
  return size;
}

bool is_bootstrap_pointer(void *ptr) noexcept {
  if (ptr == nullptr) {
    return false;
  }
  SpinLock &lock = bootstrap_lock();
  lock.lock();
  const bool found = find_bootstrap_alloc(ptr, nullptr) != nullptr;
  lock.unlock();
  return found;
}

void bootstrap_free(void *ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }

  SpinLock &lock = bootstrap_lock();
  lock.lock();
  BootstrapAlloc *prev = nullptr;
  BootstrapAlloc *alloc = find_bootstrap_alloc(ptr, &prev);
  if (alloc != nullptr) {
    if (prev == nullptr) {
      bootstrap_head() = alloc->next;
    } else {
      prev->next = alloc->next;
    }
  }
  lock.unlock();

  if (alloc != nullptr) {
    system_free(alloc, alloc->mapping_pages);
  }
}

void *bootstrap_reallocate(void *ptr, size_t size) noexcept {
  if (ptr == nullptr) {
    return bootstrap_allocate(size);
  }
  if (size == 0) {
    bootstrap_free(ptr);
    return nullptr;
  }

  const size_t old_size = bootstrap_size(ptr);
  void *next = bootstrap_allocate(size);
  if (next == nullptr) {
    return nullptr;
  }
  std::memcpy(next, ptr, std::min(old_size, size));
  bootstrap_free(ptr);
  return next;
}

void ensure_allocator_ready() noexcept {
  if (allocator_ready().load(std::memory_order_acquire) ||
      tls_initializing_allocator) {
    return;
  }

  tls_initializing_allocator = true;
  void *warm = zmalloc(8);
  zfree(warm);
  allocator_ready().store(true, std::memory_order_release);
  tls_initializing_allocator = false;
}

bool should_use_bootstrap_allocator() noexcept {
  return tls_initializing_allocator || tls_allocator_call_depth != 0 ||
         !allocator_ready().load(std::memory_order_acquire);
}

size_t managed_size(void *ptr) {
  return PageCache::get_instance().map_object_to_span(ptr)->obj_size;
}

void *allocate_bytes(size_t size) noexcept {
  if (size == 0) {
    return nullptr;
  }

  if (!allocator_ready().load(std::memory_order_acquire)) {
    ensure_allocator_ready();
  }

  if (should_use_bootstrap_allocator()) {
    return bootstrap_allocate(size);
  }

  AllocatorCallGuard guard;
  return zmalloc(size);
}

void deallocate_bytes(void *ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  if (is_bootstrap_pointer(ptr)) {
    bootstrap_free(ptr);
    return;
  }

  AllocatorCallGuard guard;
  zfree(ptr);
}

void *reallocate_bytes(void *ptr, size_t size) noexcept {
  if (ptr == nullptr) {
    return allocate_bytes(size);
  }
  if (size == 0) {
    deallocate_bytes(ptr);
    return nullptr;
  }

  if (is_bootstrap_pointer(ptr)) {
    return bootstrap_reallocate(ptr, size);
  }

  const size_t old_size = managed_size(ptr);
  void *next = allocate_bytes(size);
  if (next == nullptr) {
    return nullptr;
  }

  std::memcpy(next, ptr, std::min(old_size, size));
  deallocate_bytes(ptr);
  return next;
}

void *allocate_for_new(size_t size) {
  const size_t actual = size == 0 ? 1 : size;
  void *ptr = allocate_bytes(actual);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void *allocate_for_new_nothrow(size_t size) noexcept {
  const size_t actual = size == 0 ? 1 : size;
  return allocate_bytes(actual);
}

#if defined(__cpp_aligned_new)
struct AlignedHeader {
  void *raw;
};

void *aligned_allocate_for_new(size_t size, size_t alignment) {
  if (alignment <= alignof(std::max_align_t)) {
    return allocate_for_new(size);
  }

  const size_t actual = size == 0 ? 1 : size;
  const size_t header_size = sizeof(AlignedHeader);
  if (alignment > std::numeric_limits<size_t>::max() - actual - header_size) {
    throw std::bad_alloc();
  }

  void *raw = allocate_bytes(actual + alignment + header_size);
  if (raw == nullptr) {
    throw std::bad_alloc();
  }

  uintptr_t start = reinterpret_cast<uintptr_t>(raw) + header_size;
  uintptr_t aligned = (start + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
  auto *header =
      reinterpret_cast<AlignedHeader *>(aligned - header_size);
  header->raw = raw;
  return reinterpret_cast<void *>(aligned);
}

void *aligned_allocate_for_new_nothrow(size_t size, size_t alignment) noexcept {
  try {
    return aligned_allocate_for_new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void aligned_delete(void *ptr, size_t alignment) noexcept {
  if (ptr == nullptr) {
    return;
  }
  if (alignment <= alignof(std::max_align_t)) {
    deallocate_bytes(ptr);
    return;
  }

  const size_t header_size = sizeof(AlignedHeader);
  uintptr_t aligned = reinterpret_cast<uintptr_t>(ptr);
  auto *header =
      reinterpret_cast<AlignedHeader *>(aligned - header_size);
  deallocate_bytes(header->raw);
}
#endif

} // namespace
} // namespace internal
} // namespace zmalloc

extern "C" void *malloc(size_t size) noexcept {
  return zmalloc::internal::allocate_bytes(size);
}

extern "C" void free(void *ptr) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

extern "C" void *realloc(void *ptr, size_t size) noexcept {
  return zmalloc::internal::reallocate_bytes(ptr, size);
}

extern "C" void *calloc(size_t nmemb, size_t size) noexcept {
  size_t total = 0;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return nullptr;
  }

  void *ptr = zmalloc::internal::allocate_bytes(total);
  if (ptr != nullptr) {
    std::memset(ptr, 0, total);
  }
  return ptr;
}

extern "C" void cfree(void *ptr) noexcept {
  free(ptr);
}

void *operator new(size_t size) {
  return zmalloc::internal::allocate_for_new(size);
}

void *operator new[](size_t size) {
  return zmalloc::internal::allocate_for_new(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
  return zmalloc::internal::allocate_for_new_nothrow(size);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
  return zmalloc::internal::allocate_for_new_nothrow(size);
}

void operator delete(void *ptr) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept {
  zmalloc::internal::deallocate_bytes(ptr);
}

#if defined(__cpp_aligned_new)
void *operator new(size_t size, std::align_val_t alignment) {
  return zmalloc::internal::aligned_allocate_for_new(
      size, static_cast<size_t>(alignment));
}

void *operator new[](size_t size, std::align_val_t alignment) {
  return zmalloc::internal::aligned_allocate_for_new(
      size, static_cast<size_t>(alignment));
}

void *operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  return zmalloc::internal::aligned_allocate_for_new_nothrow(
      size, static_cast<size_t>(alignment));
}

void *operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  return zmalloc::internal::aligned_allocate_for_new_nothrow(
      size, static_cast<size_t>(alignment));
}

void operator delete(void *ptr, std::align_val_t alignment) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void *ptr, std::align_val_t alignment) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete(void *ptr, std::align_val_t alignment, size_t) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void *ptr, std::align_val_t alignment, size_t) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete(void *ptr, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void *ptr, std::align_val_t alignment,
                       const std::nothrow_t &) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}
#endif
