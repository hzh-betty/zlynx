#ifndef ZMALLOC_CONFIG_H_
#define ZMALLOC_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace zmalloc {

// 分支预测提示
#if defined(__GNUC__) || defined(__clang__)
#define ZM_LIKELY(x) (__builtin_expect(!!(x), 1))
#define ZM_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define ZM_ALWAYS_INLINE __attribute__((always_inline)) inline
#define ZM_NOINLINE __attribute__((noinline))
#else
#define ZM_LIKELY(x) (x)
#define ZM_UNLIKELY(x) (x)
#define ZM_ALWAYS_INLINE inline
#define ZM_NOINLINE
#endif

// 小于等于 MAX_BYTES 找 ThreadCache 申请，大于则找 PageCache 或系统
static constexpr size_t MAX_BYTES = 256 * 1024;

// ThreadCache 和 CentralCache 自由链表哈希桶数量
static constexpr size_t NFREELISTS = 208;

// PageCache 哈希桶数量
static constexpr size_t NPAGES = 129;

// 页大小偏移，一页 = 2^13 = 8KB
static constexpr size_t PAGE_SHIFT = 13;

// 页大小
static constexpr size_t PAGE_SIZE = static_cast<size_t>(1) << PAGE_SHIFT;

// 页号类型
using PageId = uintptr_t;

} // namespace zmalloc

#endif // ZMALLOC_CONFIG_H_
