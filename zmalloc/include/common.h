/**
 * @file common.h
 * @brief zmalloc 公共定义与常量
 */

#ifndef ZMALLOC_COMMON_H_
#define ZMALLOC_COMMON_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace zmalloc {

/// 页大小 (4KB)
static constexpr size_t kPageSize = 4096;
static constexpr size_t kPageShift = 12;

/// 最大可缓存对象大小 (32KB)
static constexpr size_t kMaxCacheableSize = 32 * 1024;

/// 每次从系统申请的最小页数
static constexpr size_t kMinSystemAllocPages = 128;

/// SizeClass 数量
static constexpr size_t kNumSizeClasses = 160;

/// ThreadCache 最大缓存大小 (2MB)
static constexpr size_t kMaxThreadCacheSize = 2 * 1024 * 1024;

/// 对齐辅助函数
inline size_t AlignUp(size_t n, size_t alignment) {
  return (n + alignment - 1) & ~(alignment - 1);
}

/// 计算大小对应的页数
inline size_t SizeToPages(size_t size) {
  return (size + kPageSize - 1) >> kPageShift;
}

/// 计算页数对应的字节数
inline size_t PagesToSize(size_t pages) { return pages << kPageShift; }

/// 从系统申请内存
inline void *SystemAlloc(size_t size) {
  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  return ptr;
}

/// 归还内存给系统
inline void SystemFree(void *ptr, size_t size) { munmap(ptr, size); }

} // namespace zmalloc

#endif // ZMALLOC_COMMON_H_
