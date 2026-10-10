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
#else
#define ZM_LIKELY(x) (x)
#define ZM_UNLIKELY(x) (x)
#define ZM_ALWAYS_INLINE inline
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

// 跨缓存层批量搬运对象时使用的统一上限。
static constexpr size_t MAX_BATCH_SIZE = 128;

// SizeClass 计算建议批量时使用的目标传输字节数。
static constexpr size_t SIZE_CLASS_TRANSFER_BYTES = 64 * 1024;

// CentralCache 单次归还处理中允许建立的最大 Span 分组数。
static constexpr size_t CENTRAL_RELEASE_GROUPS = 128;

// 每线程缓存的软字节预算。
static constexpr size_t THREAD_CACHE_BUDGET = 1024 * 1024;
static constexpr unsigned THREAD_CACHE_MAX_OVERAGES = 3;

// TransferCache 每个大小类的物理槽位上限和目标字节预算（至少一批）。
static constexpr size_t TRANSFER_CACHE_SLOTS = 2048;
static constexpr size_t TRANSFER_CACHE_BUDGET = 64 * 1024;

// ObjectPool 每次向系统申请的内存块大小。
static constexpr size_t OBJECT_POOL_BLOCK_SIZE = 128 * 1024;

// 共享缓存中用于隔离热点锁的缓存行大小。
static constexpr size_t CACHE_LINE_SIZE = 64;

static_assert((TRANSFER_CACHE_SLOTS & (TRANSFER_CACHE_SLOTS - 1)) == 0,
              "TRANSFER_CACHE_SLOTS must be power of two");
static_assert(OBJECT_POOL_BLOCK_SIZE % PAGE_SIZE == 0,
              "OBJECT_POOL_BLOCK_SIZE must contain whole pages");

// 页号类型
using PageId = uintptr_t;

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_CONFIG_H_
