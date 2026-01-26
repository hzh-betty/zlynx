/**
 * @file size_class_lookup.cc
 * @brief SizeClass 查表初始化
 *
 * 说明：
 * - 参考 tcmalloc 的 SizeMap 思路，把“按 size 计算对齐/索引/批量策略”的逻辑
 *   从热路径挪到初始化阶段。
 * - 热路径直接按 8 字节分桶查表，减少分支与除法。
 */

#include "common.h"

namespace zmalloc {

SizeClassLookup g_size_class_lookup[kSizeClassLookupLen];

namespace {

static inline size_t clamp_min(size_t v, size_t min_v) {
  return v < min_v ? min_v : v;
}

static inline size_t clamp_max(size_t v, size_t max_v) {
  return v > max_v ? max_v : v;
}

static void init_size_class_lookup() {
  // bucket=0 对应 size==0，保持为 0。
  g_size_class_lookup[0] = {0u, 0u, 0u, 0u};

  for (size_t bucket = 1; bucket < kSizeClassLookupLen; ++bucket) {
    const size_t bytes = bucket * 8;

    // 计算对齐后的大小与桶索引（只在初始化阶段跑一次）。
    const size_t align_size = SizeClass::round_up(bytes);
    const size_t index = SizeClass::index(align_size);

    // 预计算批量搬运个数/页数（与 SizeClass::num_move_* 的策略一致）。
    constexpr size_t kTargetBytes = 4096;
    constexpr size_t kMinObjects = 2;
    constexpr size_t kMaxObjects = 128;

    size_t num_move = kTargetBytes / align_size;
    num_move = clamp_min(num_move, kMinObjects);
    num_move = clamp_max(num_move, kMaxObjects);

    size_t num_pages = (num_move * align_size) >> PAGE_SHIFT;
    if (num_pages == 0) {
      num_pages = 1;
    }

    SizeClassLookup e;
    e.align_size = static_cast<uint32_t>(align_size);
    e.index = static_cast<uint16_t>(index);
    e.num_move = static_cast<uint16_t>(num_move);
    e.num_pages = static_cast<uint16_t>(num_pages);
    g_size_class_lookup[bucket] = e;
  }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void zmalloc_init_tables() {
  // 进程级初始化：构建 SizeClass 查表。
  init_size_class_lookup();
}

} // namespace

} // namespace zmalloc
