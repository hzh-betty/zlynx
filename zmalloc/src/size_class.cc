/**
 * @file size_class.cc
 * @brief SizeClass 成员函数实现
 */

#include "common.h"

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

} // namespace zmalloc
