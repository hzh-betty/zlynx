#ifndef ZMALLOC_SIZE_CLASS_H_
#define ZMALLOC_SIZE_CLASS_H_

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "zmalloc_config.h"

namespace zmalloc {

static constexpr size_t kSizeClassLookupLen = (MAX_BYTES / 8) + 1;

struct SizeClassLookup {
  uint32_t align_size;
  uint16_t index;
  uint16_t num_move;
  uint16_t num_pages;
};

extern SizeClassLookup g_size_class_lookup[kSizeClassLookupLen];

/**
 * @brief 大小类，管理对齐和映射关系
 */
class SizeClass {
public:
  static size_t round_up(size_t bytes, size_t align_num);
  static size_t round_up(size_t bytes);
  static size_t index(size_t bytes, size_t align_shift);
  static size_t index(size_t bytes);
  static size_t num_move_size(size_t size);
  static size_t num_move_page(size_t size);

  static inline const SizeClassLookup &lookup(size_t bytes);

  static inline size_t round_up_fast(size_t bytes) {
    return static_cast<size_t>(lookup(bytes).align_size);
  }

  static inline size_t index_fast(size_t bytes) {
    return static_cast<size_t>(lookup(bytes).index);
  }

  static inline void classify(size_t bytes, size_t &align_size, size_t &index) {
    const SizeClassLookup &e = lookup(bytes);
    align_size = static_cast<size_t>(e.align_size);
    index = static_cast<size_t>(e.index);
  }
};

inline const SizeClassLookup &SizeClass::lookup(size_t bytes) {
  if (ZM_UNLIKELY(bytes == 0)) {
    return g_size_class_lookup[0];
  }
  assert(bytes <= MAX_BYTES);
  const size_t bucket = (bytes + 7) >> 3;
  return g_size_class_lookup[bucket];
}

} // namespace zmalloc

#endif // ZMALLOC_SIZE_CLASS_H_
