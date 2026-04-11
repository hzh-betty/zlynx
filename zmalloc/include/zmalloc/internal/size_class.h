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
    uint16_t index; // 对应 central cache / freelist 的大小类索引。
    uint16_t num_move; // thread cache 一次向 central 申请/归还的对象个数。
    uint16_t num_pages; // central 向 page cache 申请 span 时的页数建议值。
};

extern SizeClassLookup g_size_class_lookup[kSizeClassLookupLen];
extern std::atomic<bool> g_size_class_lookup_ready;

// 懒初始化入口：首次查表时构建完整映射，后续只读访问。
void init_size_class_lookup_once();

/**
 * @brief 大小类策略与查找接口
 *
 * 该类统一维护“请求字节数 -> 对齐尺寸/大小类索引”的映射，
 * 并提供慢路径（通用计算）与快路径（查表）两套接口。
 */
class SizeClass {
  public:
    static size_t round_up(size_t bytes, size_t align_num);
    static size_t round_up(size_t bytes);
    static size_t index(size_t bytes, size_t align_shift);
    static size_t index(size_t bytes);
    static size_t num_move_size(size_t size);
    static size_t num_move_page(size_t size);

    // 返回 bytes 对应的预计算条目；bytes=0 映射到保留槽位 0。
    static inline const SizeClassLookup &lookup(size_t bytes);

    static inline size_t round_up_fast(size_t bytes) {
        return static_cast<size_t>(lookup(bytes).align_size);
    }

    static inline size_t index_fast(size_t bytes) {
        return static_cast<size_t>(lookup(bytes).index);
    }

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
