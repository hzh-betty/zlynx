/**
 * @file override.cc
 * @brief override 实现。
 * @author hzh-betty
 */

#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/system_alloc.h"
#include "zmalloc/zmalloc.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#if defined(__GLIBC__)
#include <dlfcn.h>
#endif

namespace zmalloc {
namespace internal {

namespace {

// override.cc 负责把 libc/new/delete 入口统一接到 zmalloc：
// - 初始化早期或递归进入分配器时，先走 bootstrap allocator 保底。
// - 正常阶段优先走 zmalloc 管理路径。
// - 对非 zmalloc 指针提供保守降级（glibc free/realloc）。

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

// 递归保护：malloc/new 可能在初始化或日志路径里再次触发分配。
// call_depth>0 时强制走 bootstrap，避免 re-enter 主分配器导致死锁。
thread_local bool tls_initializing_allocator = false;

size_t bootstrap_mapping_pages(size_t size) {
    // bootstrap 元信息和用户区放在同一块映射里，便于一次 system_free 回收。
    size_t total = 0;
    if (__builtin_add_overflow(sizeof(BootstrapAlloc), size, &total) ||
        __builtin_add_overflow(total, PAGE_SIZE - 1, &total)) {
        return 0;
    }
    return total >> PAGE_SHIFT;
}

void *bootstrap_allocate(size_t size) noexcept {
    if (size == 0) {
        return nullptr;
    }

    const size_t pages = bootstrap_mapping_pages(size);
    BootstrapAlloc *alloc =
        static_cast<BootstrapAlloc *>(system_alloc_nothrow(pages));
    if (alloc == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    // user_ptr 紧跟在元信息之后，避免额外地址映射结构。
    alloc->user_ptr = alloc + 1;
    alloc->user_size = size;
    alloc->mapping_pages = pages;

    SpinLock &lock = bootstrap_lock();
    lock.lock();
    alloc->next = bootstrap_head();
    bootstrap_head() = alloc;
    lock.unlock();
    return alloc->user_ptr;
}

BootstrapAlloc *find_bootstrap_alloc(void *ptr,
                                     BootstrapAlloc **prev_out) noexcept {
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
        // bootstrap 映射不走 span 管理，直接按记录页数归还给 system allocator。
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

void ensure_allocator_ready() {
    if (allocator_ready().load(std::memory_order_acquire) ||
        tls_initializing_allocator) {
        return;
    }

    tls_initializing_allocator = true;
    // 热身一次主路径，确保后续重入判断可依赖 ready 标记。
    try {
        void *warm = zmalloc(8);
        zfree(warm);
        allocator_ready().store(true, std::memory_order_release);
    } catch (...) {
        tls_initializing_allocator = false;
        throw;
    }
    tls_initializing_allocator = false;
}

bool should_use_bootstrap_allocator() noexcept {
    // 三类场景统一落到 bootstrap：
    // 1) 正在做 ready 初始化；2) 当前线程已在分配调用栈中；3) allocator 尚未
    // ready。
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
    const uintptr_t span_begin = static_cast<uintptr_t>(span->page_id)
                                 << PAGE_SHIFT;
    const uintptr_t span_end = span_begin + (span->n << PAGE_SHIFT);
    if (addr < span_begin || addr >= span_end) {
        return nullptr;
    }

    const uintptr_t offset = addr - span_begin;
    if (span->obj_size > MAX_BYTES) {
        // 大对象按整 span 管理，只允许块首地址作为合法用户指针。
        return offset == 0 ? span : nullptr;
    }

    if (span->obj_size == 0 || offset % span->obj_size != 0) {
        // 小对象必须落在切分粒度边界上，避免内部地址误判成可释放指针。
        return nullptr;
    }

    return span;
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
    if (!bootstrap_contains_address(header_addr) &&
        (!allocator_ready().load(std::memory_order_acquire) ||
         PageCache::get_instance().try_map_object_to_span(header_addr) ==
             nullptr)) {
        return false;
    }

    auto *header = reinterpret_cast<AlignedHeader *>(header_addr);
    if (header->magic != kAlignedAllocMagic || header->raw == nullptr) {
        return false;
    }

    void *raw = header->raw;
    if (!is_bootstrap_pointer(raw) &&
        (!allocator_ready().load(std::memory_order_acquire) ||
         managed_span(raw) == nullptr)) {
        // header 看起来合法，但 raw 不受管时拒绝解包，防止误解引用外部内存。
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
    if (size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
        errno = ENOMEM;
        return nullptr;
    }
    const size_t actual = size == 0 ? 1 : size;

    try {
        if (should_use_bootstrap_allocator()) {
            if (tls_initializing_allocator || tls_allocator_call_depth != 0) {
                return bootstrap_allocate(actual);
            }
            ensure_allocator_ready();
        }

        // guard 生命周期覆盖 zmalloc 调用，防止调用栈内部再次分配时重入主路径。
        AllocatorCallGuard guard;
        return zmalloc(actual);
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return nullptr;
    }
}

void *aligned_allocate_bytes(size_t size, size_t alignment) noexcept {
    if (!is_power_of_two(alignment)) {
        errno = EINVAL;
        return nullptr;
    }
    if (alignment <= alignof(std::max_align_t)) {
        return allocate_bytes(size == 0 ? 1 : size);
    }

    const size_t actual = size == 0 ? 1 : size;
    const size_t header_size = sizeof(AlignedHeader);
    size_t total = 0;
    if (__builtin_add_overflow(actual, alignment - 1, &total) ||
        __builtin_add_overflow(total, header_size, &total)) {
        errno = ENOMEM;
        return nullptr;
    }

    void *raw = allocate_bytes(total);
    if (raw == nullptr) {
        return nullptr;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(raw) + header_size;
    uintptr_t aligned =
        (start + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
    // 在对齐地址前塞回溯头，free/realloc 可从用户指针逆向找回 raw。
    auto *header = reinterpret_cast<AlignedHeader *>(aligned - header_size);
    header->magic = kAlignedAllocMagic;
    header->raw = raw;
    header->user_size = actual;

    if (allocator_ready().load(std::memory_order_acquire)) {
        PageCache &pc = PageCache::get_instance();
        Span *span = pc.try_map_object_to_span(raw);
        if (span != nullptr && span->n > NPAGES - 1) {
            AllocatorCallGuard guard;
            try {
                std::lock_guard<std::mutex> lock(pc.page_mtx());
                // 普通大块仍只登记起始页；对齐块额外登记回溯头和用户页。
                pc.map_span_page(span, header);
                pc.map_span_page(span, reinterpret_cast<void *>(aligned));
            } catch (const std::bad_alloc &) {
                zfree(raw);
                errno = ENOMEM;
                return nullptr;
            }
        }
    }
    return reinterpret_cast<void *>(aligned);
}

void deallocate_bytes(void *ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    const bool ready = allocator_ready().load(std::memory_order_acquire);
    if (is_bootstrap_pointer(ptr)) {
        bootstrap_free(ptr);
        return;
    }

    if ((ready || (!tls_initializing_allocator &&
                   tls_allocator_call_depth == 0)) &&
        managed_span(ptr) != nullptr) {
        AllocatorCallGuard guard;
        zfree(ptr);
        return;
    }

    void *aligned_raw = nullptr;
    if (unwrap_aligned_pointer(ptr, &aligned_raw, nullptr)) {
        // aligned 指针对应的真实分配块仍由 raw 管理，递归回收 raw 即可。
        deallocate_bytes(aligned_raw);
        return;
    }
#if defined(__GLIBC__)
    // 非受管指针兜底交回 glibc，避免破坏外部分配器对象生命周期。
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
    if (size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
        errno = ENOMEM;
        return nullptr;
    }

    if (is_bootstrap_pointer(ptr)) {
        return bootstrap_reallocate(ptr, size);
    }

    Span *span = nullptr;
    if (allocator_ready().load(std::memory_order_acquire) ||
        (!tls_initializing_allocator && tls_allocator_call_depth == 0)) {
        span = managed_span(ptr);
    }
    if (span != nullptr) {
        // 分配新块、拷贝并释放旧块。
        const size_t old_size = span->obj_size;
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
        // realloc 返回满足默认对齐的地址，不保留原始扩展对齐。
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

size_t usable_size(void *ptr) noexcept {
    if (ptr == nullptr) {
        return 0;
    }
    if (allocator_ready().load(std::memory_order_acquire)) {
        if (Span *span = managed_span(ptr)) {
            return span->obj_size;
        }
    }
    const size_t boot_size = bootstrap_size(ptr);
    if (boot_size != 0) {
        return boot_size;
    }
    size_t aligned_size = 0;
    if (unwrap_aligned_pointer(ptr, nullptr, &aligned_size)) {
        return aligned_size;
    }
#if defined(__GLIBC__)
    // 仅外部 glibc 块需要动态查找；查找期间的递归分配由 bootstrap 处理。
    AllocatorCallGuard guard;
    using UsableSize = size_t (*)(void *);
    static UsableSize libc_usable_size =
        reinterpret_cast<UsableSize>(dlsym(RTLD_NEXT, "malloc_usable_size"));
    return libc_usable_size == nullptr ? 0 : libc_usable_size(ptr);
#else
    return 0;
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

#endif

} // namespace
} // namespace internal
} // namespace zmalloc

extern "C" void *malloc(size_t size) noexcept {
    return zmalloc::internal::allocate_bytes(size);
}

extern "C" void free(void *ptr) noexcept {
    const int saved_errno = errno;
    zmalloc::internal::deallocate_bytes(ptr);
    errno = saved_errno;
}

extern "C" size_t malloc_usable_size(void *ptr) noexcept {
    return zmalloc::internal::usable_size(ptr);
}

extern "C" void *realloc(void *ptr, size_t size) noexcept {
    return zmalloc::internal::reallocate_bytes(ptr, size);
}

extern "C" void *calloc(size_t nmemb, size_t size) noexcept {
    size_t total = 0;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        errno = ENOMEM;
        return nullptr;
    }

    void *ptr = zmalloc::internal::allocate_bytes(total);
    if (ptr != nullptr) {
        std::memset(ptr, 0, total);
    }
    return ptr;
}

extern "C" void cfree(void *ptr) noexcept { free(ptr); }

extern "C" void *memalign(size_t alignment, size_t size) noexcept {
    return zmalloc::internal::aligned_allocate_bytes(size, alignment);
}

extern "C" void *aligned_alloc(size_t alignment, size_t size) noexcept {
    if (alignment == 0 || !zmalloc::internal::is_power_of_two(alignment) ||
        (size % alignment) != 0) {
        errno = EINVAL;
        return nullptr;
    }
    return zmalloc::internal::aligned_allocate_bytes(size, alignment);
}

extern "C" int posix_memalign(void **memptr, size_t alignment,
                              size_t size) noexcept {
    if (alignment < sizeof(void *) ||
        !zmalloc::internal::is_power_of_two(alignment)) {
        return EINVAL;
    }

    const int saved_errno = errno;
    void *ptr = zmalloc::internal::aligned_allocate_bytes(size, alignment);
    errno = saved_errno;
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
        errno = ENOMEM;
        return nullptr;
    }
    // pvalloc 语义：按页向上取整后再返回页对齐地址。
    rounded &= ~(zmalloc::PAGE_SIZE - 1);
    if (rounded == 0) {
        rounded = zmalloc::PAGE_SIZE;
    }
    return zmalloc::internal::aligned_allocate_bytes(rounded,
                                                     zmalloc::PAGE_SIZE);
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

void operator delete(void *ptr, std::align_val_t) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr, std::align_val_t) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete(void *ptr, size_t, std::align_val_t) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr, size_t, std::align_val_t) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete(void *ptr, std::align_val_t,
                     const std::nothrow_t &) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}

void operator delete[](void *ptr, std::align_val_t,
                       const std::nothrow_t &) noexcept {
    zmalloc::internal::deallocate_bytes(ptr);
}
#endif
