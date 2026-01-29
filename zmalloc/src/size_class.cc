/**
 * @file size_class.cc
 * @brief SizeClass 成员函数实现
 */

#include "size_class.h"

namespace zmalloc {

size_t SizeClass::round_up(size_t bytes, size_t align_num) {
  return (bytes + align_num - 1) & ~(align_num - 1);
}

size_t SizeClass::round_up(size_t bytes) {
  if (bytes <= 128) {
    return round_up(bytes, 8);
  } else if (bytes <= 1024) {
    return round_up(bytes, 16);
  } else if (bytes <= 8 * 1024) {
    return round_up(bytes, 128);
  } else if (bytes <= 64 * 1024) {
    return round_up(bytes, 1024);
  } else if (bytes <= 256 * 1024) {
    return round_up(bytes, 8 * 1024);
  } else {
    // 大于 256KB 按页对齐
    return round_up(bytes, PAGE_SIZE);
  }
}

size_t SizeClass::index(size_t bytes, size_t align_shift) {
  return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
}

size_t SizeClass::index(size_t bytes) {
  // 每个区间的桶数量
  static constexpr size_t kGroupArray[4] = {16, 56, 56, 56};
  if (bytes <= 128) {
    return index(bytes, 3);
  } else if (bytes <= 1024) {
    return index(bytes - 128, 4) + kGroupArray[0];
  } else if (bytes <= 8 * 1024) {
    return index(bytes - 1024, 7) + kGroupArray[0] + kGroupArray[1];
  } else if (bytes <= 64 * 1024) {
    return index(bytes - 8 * 1024, 10) + kGroupArray[0] + kGroupArray[1] +
           kGroupArray[2];
  } else if (bytes <= 256 * 1024) {
    return index(bytes - 64 * 1024, 13) + kGroupArray[0] + kGroupArray[1] +
           kGroupArray[2] + kGroupArray[3];
  } else {
    assert(false);
    return static_cast<size_t>(-1);
  }
}

size_t SizeClass::num_move_size(size_t size) {
  assert(size > 0);

  // 用“目标传输字节数”来决定每次批量对象个数。
  constexpr size_t kTargetBytes = 4096;
  constexpr size_t kMinObjects = 2;
  constexpr size_t kMaxObjects = 128;

  size_t num = kTargetBytes / size;
  if (num < kMinObjects) {
    num = kMinObjects;
  }
  if (num > kMaxObjects) {
    num = kMaxObjects;
  }
  return num;
}

size_t SizeClass::num_move_page(size_t size) {
  size_t num = num_move_size(size);
  size_t npage = (num * size) >> PAGE_SHIFT;
  if (npage == 0)
    npage = 1;
  return npage;
}


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
  // 注意：这是“尽早”初始化手段（constructor attribute）。若未来希望更可控
  //（例如显式 init），可替换为函数式初始化并在入口处调用。
  init_size_class_lookup();
}

} // namespace

} // namespace zmalloc
