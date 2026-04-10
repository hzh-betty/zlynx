#include "page_cache.h"
#include "system_alloc.h"
#include "zmalloc.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <atomic>
#include <cstdint>

namespace zmalloc {
namespace internal {

namespace {

#if defined(__GLIBC__)
extern "C" void __libc_free(void *ptr) noexcept;
extern "C" void *__libc_realloc(void *ptr, size_t size) noexcept;
#endif

struct BootstrapAlloc {
  BootstrapAlloc *next;
  void *user_ptr;
  size_t user_size;
  size_t mapping_pages;
};

struct AlignedHeader {
  uint64_t magic;
  void *raw;
  size_t user_size;
};

constexpr uint64_t kAlignedAllocMagic = 0x5a6d616c6c6f6341ULL;

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

bool bootstrap_contains_address(void *ptr) noexcept {
  if (ptr == nullptr) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  SpinLock &lock = bootstrap_lock();
  lock.lock();
  BootstrapAlloc *cur = bootstrap_head();
  while (cur != nullptr) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(cur->user_ptr);
    const uintptr_t end = begin + cur->user_size;
    if (addr >= begin && addr < end) {
      lock.unlock();
      return true;
    }
    cur = cur->next;
  }
  lock.unlock();
  return false;
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

bool is_power_of_two(size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

Span *managed_span(void *ptr) {
  Span *span = PageCache::get_instance().try_map_object_to_span(ptr);
  if (span == nullptr || !span->is_use) {
    return nullptr;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t span_begin = static_cast<uintptr_t>(span->page_id) << PAGE_SHIFT;
  const uintptr_t span_end = span_begin + (span->n << PAGE_SHIFT);
  if (addr < span_begin || addr >= span_end) {
    return nullptr;
  }

  const uintptr_t offset = addr - span_begin;
  if (span->obj_size > MAX_BYTES) {
    return offset == 0 ? span : nullptr;
  }

  if (span->obj_size == 0 || offset % span->obj_size != 0) {
    return nullptr;
  }

  return span;
}

size_t managed_size(void *ptr) {
  return managed_span(ptr)->obj_size;
}

bool unwrap_aligned_pointer(void *ptr, void **raw_out,
                            size_t *size_out) noexcept {
  if (ptr == nullptr) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  if (addr < sizeof(AlignedHeader)) {
    return false;
  }

  void *header_addr = reinterpret_cast<void *>(addr - sizeof(AlignedHeader));
  if (PageCache::get_instance().try_map_object_to_span(header_addr) == nullptr &&
      !bootstrap_contains_address(header_addr)) {
    return false;
  }

  auto *header = reinterpret_cast<AlignedHeader *>(header_addr);
  if (header->magic != kAlignedAllocMagic || header->raw == nullptr) {
    return false;
  }

  void *raw = header->raw;
  if (!is_bootstrap_pointer(raw) && managed_span(raw) == nullptr) {
    return false;
  }

  if (raw_out != nullptr) {
    *raw_out = raw;
  }
  if (size_out != nullptr) {
    *size_out = header->user_size;
  }
  return true;
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

void *aligned_allocate_bytes(size_t size, size_t alignment) noexcept {
  if (alignment <= alignof(std::max_align_t)) {
    return allocate_bytes(size == 0 ? 1 : size);
  }

  if (!is_power_of_two(alignment)) {
    return nullptr;
  }

  const size_t actual = size == 0 ? 1 : size;
  const size_t header_size = sizeof(AlignedHeader);
  if (alignment > std::numeric_limits<size_t>::max() - actual - header_size) {
    return nullptr;
  }

  void *raw = allocate_bytes(actual + alignment + header_size);
  if (raw == nullptr) {
    return nullptr;
  }

  uintptr_t start = reinterpret_cast<uintptr_t>(raw) + header_size;
  uintptr_t aligned =
      (start + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
  auto *header = reinterpret_cast<AlignedHeader *>(aligned - header_size);
  header->magic = kAlignedAllocMagic;
  header->raw = raw;
  header->user_size = actual;
  return reinterpret_cast<void *>(aligned);
}

void deallocate_bytes(void *ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  if (is_bootstrap_pointer(ptr)) {
    bootstrap_free(ptr);
    return;
  }

  if (managed_span(ptr) != nullptr) {
    AllocatorCallGuard guard;
    zfree(ptr);
    return;
  }

  void *aligned_raw = nullptr;
  if (unwrap_aligned_pointer(ptr, &aligned_raw, nullptr)) {
    deallocate_bytes(aligned_raw);
    return;
  }
#if defined(__GLIBC__)
  __libc_free(ptr);
  return;
#else
  return;
#endif
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

  if (managed_span(ptr) != nullptr) {
    const size_t old_size = managed_size(ptr);
    void *next = allocate_bytes(size);
    if (next == nullptr) {
      return nullptr;
    }

    std::memcpy(next, ptr, std::min(old_size, size));
    deallocate_bytes(ptr);
    return next;
  }

  void *aligned_raw = nullptr;
  size_t aligned_size = 0;
  if (unwrap_aligned_pointer(ptr, &aligned_raw, &aligned_size)) {
    void *next = aligned_allocate_bytes(size, alignof(std::max_align_t));
    if (next == nullptr) {
      return nullptr;
    }
    std::memcpy(next, ptr, std::min(aligned_size, size));
    deallocate_bytes(aligned_raw);
    return next;
  }
#if defined(__GLIBC__)
  return __libc_realloc(ptr, size);
#else
  return nullptr;
#endif
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
void *aligned_allocate_for_new(size_t size, size_t alignment) {
  void *ptr = aligned_allocate_bytes(size, alignment);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return ptr;
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
  deallocate_bytes(ptr);
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

extern "C" void *memalign(size_t alignment, size_t size) noexcept {
  return zmalloc::internal::aligned_allocate_bytes(size, alignment);
}

extern "C" void *aligned_alloc(size_t alignment, size_t size) noexcept {
  if (alignment == 0 || !zmalloc::internal::is_power_of_two(alignment) ||
      (size % alignment) != 0) {
    return nullptr;
  }
  return zmalloc::internal::aligned_allocate_bytes(size, alignment);
}

extern "C" int posix_memalign(void **memptr, size_t alignment,
                               size_t size) noexcept {
  // glibc 头文件把 memptr 标成 nonnull；转成整数后再判零，保留防御性检查，
  // 同时避免编译器把这里视为恒假比较。
  if (reinterpret_cast<std::uintptr_t>(memptr) == 0) {
    return EINVAL;
  }
  *memptr = nullptr;
  if (alignment < sizeof(void *) ||
      !zmalloc::internal::is_power_of_two(alignment)) {
    return EINVAL;
  }

  void *ptr = zmalloc::internal::aligned_allocate_bytes(size, alignment);
  if (ptr == nullptr) {
    return ENOMEM;
  }
  *memptr = ptr;
  return 0;
}

extern "C" void *valloc(size_t size) noexcept {
  return zmalloc::internal::aligned_allocate_bytes(size, zmalloc::PAGE_SIZE);
}

extern "C" void *pvalloc(size_t size) noexcept {
  size_t rounded = 0;
  if (__builtin_add_overflow(size, zmalloc::PAGE_SIZE - 1, &rounded)) {
    return nullptr;
  }
  rounded &= ~(zmalloc::PAGE_SIZE - 1);
  return zmalloc::internal::aligned_allocate_bytes(rounded, zmalloc::PAGE_SIZE);
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

void operator delete(void *ptr, size_t,
                     std::align_val_t alignment) noexcept {
  zmalloc::internal::aligned_delete(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void *ptr, size_t,
                       std::align_val_t alignment) noexcept {
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
