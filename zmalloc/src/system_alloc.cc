/**
 * @file system_alloc.cc
 * @brief 系统内存分配：按需处理对齐，不重试地址 hint
 * @author hzh-betty
 */

#include "zmalloc/internal/system_alloc.h"

#include <sys/mman.h>

#include <cstdint>
#include <cerrno>
#include <limits>
#include <new>

#include "zmalloc/internal/zmalloc_config.h"

namespace zmalloc {

void *system_alloc(size_t kpage) {
    void *ptr = system_alloc_nothrow(kpage);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void *system_alloc_nothrow(size_t kpage) noexcept {
    // 额外一页用于对齐；先检查移位和加法，不能因溢出映射过小区域。
    if (kpage == 0 ||
        kpage > (std::numeric_limits<size_t>::max() - PAGE_SIZE) / PAGE_SIZE) {
        errno = ENOMEM;
        return nullptr;
    }
    const size_t size = kpage * PAGE_SIZE;
    // 优先保留内核给出的完整映射，避免无条件裁剪留下地址空洞。
    void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    if ((reinterpret_cast<uintptr_t>(ptr) & (PAGE_SIZE - 1)) == 0) {
        return ptr;
    }

    // 内核页对齐不一定满足分配器的 8 KiB 对齐；仅在此时扩大映射。
    munmap(ptr, size);
    const size_t reserved = size + PAGE_SIZE;
    ptr = mmap(nullptr, reserved, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    // 预留的是完整请求区间，不再用一个小空洞猜测大区间是否可用。
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t aligned = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    const size_t prefix = aligned - addr;
    const size_t suffix = reserved - prefix - size;
    if (prefix != 0) {
        munmap(ptr, prefix);
    }
    if (suffix != 0) {
        munmap(reinterpret_cast<void *>(aligned + size), suffix);
    }
    return reinterpret_cast<void *>(aligned);
}

void system_free(void *ptr, size_t kpage) { munmap(ptr, kpage << PAGE_SHIFT); }

bool system_release(void *ptr, size_t kpage) noexcept {
    // 仅接受分配器整页范围，防止长度或地址溢出后建议回收错误区间。
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    if (ptr == nullptr || kpage == 0 || (address & (PAGE_SIZE - 1)) != 0 ||
        kpage > std::numeric_limits<size_t>::max() / PAGE_SIZE ||
        address > std::numeric_limits<uintptr_t>::max() - kpage * PAGE_SIZE) {
        errno = EINVAL;
        return false;
    }
    return madvise(ptr, kpage * PAGE_SIZE, MADV_DONTNEED) == 0;
}

} // namespace zmalloc
