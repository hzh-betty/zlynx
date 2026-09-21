/**
 * @file zmalloc_config.h
 * @brief zmalloc_config 定义。
 * @author
 * hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_CONFIG_H_
#define ZMALLOC_INTERNAL_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace zmalloc {

// 分支预测提示：只影响编译器布局，不改变条件表达式的结果。
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

// 小对象阈值；不超过该值的请求由 ThreadCache/CentralCache 管理。
static constexpr size_t MAX_BYTES = 256 * 1024;

// 大小类数量，同时也是 ThreadCache、CentralCache 和 TransferCache 的桶数。
static constexpr size_t NFREELISTS = 208;

// PageCache 按 Span 页数索引的桶数（下标 0 保留不用）。
static constexpr size_t NPAGES = 129;

// 页大小偏移，一页 = 2^13 = 8KB
static constexpr size_t PAGE_SHIFT = 13;

// 页大小，由 PAGE_SHIFT 定义，所有页号/地址换算共用此值。
static constexpr size_t PAGE_SIZE = static_cast<size_t>(1) << PAGE_SHIFT;

// 页号类型
using PageId = uintptr_t;

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_CONFIG_H_
