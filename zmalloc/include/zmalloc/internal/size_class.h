/**
 * @file size_class.h
 * @brief size_class 定义。
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_SIZE_CLASS_H_
#define ZMALLOC_INTERNAL_SIZE_CLASS_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "zmalloc_config.h"

namespace zmalloc {

static constexpr size_t kSizeClassLookupLen = (MAX_BYTES / 8) + 1;

// g_size_class_lookup 的每一项都对应一个 8-byte bucket。
// 预计算后，小对象分配路径只需一次数组访问即可拿到完整分类信息，
// 避免在热路径里反复做分支和除法。
struct SizeClassLookup {
    uint32_t align_size; // 当前 bucket 实际对齐后的对象大小。
    uint16_t index;      // 对应 central cache / freelist 的大小类索引。
    uint16_t num_move;   // thread cache 一次向 central 申请/归还的对象个数。
    uint16_t num_pages;  // central 向 page cache 申请 span 时的页数建议值。
};

extern SizeClassLookup g_size_class_lookup[kSizeClassLookupLen];
extern std::atomic<bool> g_size_class_lookup_ready;

/**
 * @brief 线程安全地初始化请求大小到大小类信息的只读查找表
 *
 * 首次调用构建完整映射，之后只进行只读访问。
 */
void init_size_class_lookup_once();

/**
 * @brief 大小类策略与查找接口
 *
 * 该类维护“请求字节数 -> 对齐尺寸/大小类索引”的映射。较小请求
 * 使用更细的对齐粒度以减少内部碎片，较大请求逐步增大粒度，以限制
 * 大小类总数。
 *
 * 热路径通过 g_size_class_lookup 一次取得对齐尺寸、索引、批量对象数
 * 和建议 Span 页数。表只初始化一次，发布后保持只读，可无锁访问。
 */
class SizeClass {
  public:
    /** @brief 将字节数向上对齐到指定对齐值（对齐值须为 2 的幂）。 */
    static size_t round_up(size_t bytes, size_t align_num);
    /** @brief 按分配器分段策略对齐请求大小。 */
    static size_t round_up(size_t bytes);
    /** @brief 按给定对齐位数计算从零开始的大小类索引。 */
    static size_t index(size_t bytes, size_t align_shift);
    /** @brief 按分配器分段策略计算大小类索引。 */
    static size_t index(size_t bytes);
    /** @brief 计算线程缓存与中心缓存间建议的一次批量对象数。 */
    static size_t num_move_size(size_t size);
    /** @brief 计算至少容纳建议批量对象所需的页数。 */
    static size_t num_move_page(size_t size);

    /** @brief 返回 bytes 对应的预计算条目；0 映射到保留槽位 0。 */
    static inline const SizeClassLookup &lookup(size_t bytes);

    /** @brief 使用预计算表快速取得对齐后的对象大小。 */
    static inline size_t round_up_fast(size_t bytes) {
        return static_cast<size_t>(lookup(bytes).align_size);
    }

    /** @brief 使用预计算表快速取得大小类索引。 */
    static inline size_t index_fast(size_t bytes) {
        return static_cast<size_t>(lookup(bytes).index);
    }

    /** @brief 一次查表同时取得对齐尺寸和大小类索引。 */
    static inline void classify(size_t bytes, size_t &align_size,
                                size_t &index) {
        const SizeClassLookup &e = lookup(bytes);
        align_size = static_cast<size_t>(e.align_size);
        index = static_cast<size_t>(e.index);
    }
};

inline const SizeClassLookup &SizeClass::lookup(size_t bytes) {
    if (ZM_UNLIKELY(
            !g_size_class_lookup_ready.load(std::memory_order_acquire))) {
        // acquire/release 与初始化端配合，保证表项内容可见。
        init_size_class_lookup_once();
    }
    if (ZM_UNLIKELY(bytes == 0)) {
        // 0 字节请求统一映射到哨兵条目，避免后续出现负索引或越界。
        return g_size_class_lookup[0];
    }
    assert(bytes <= MAX_BYTES);
    // 每 8 字节一个 bucket，向上取整映射到查表下标。
    const size_t bucket = (bytes + 7) >> 3;
    return g_size_class_lookup[bucket];
}

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_SIZE_CLASS_H_
