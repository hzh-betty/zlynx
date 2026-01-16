/**
 * @file size_class.cc
 * @brief SizeClass 实现
 */

#include "size_class.h"

#include <algorithm>

namespace zmalloc {

SizeClass &SizeClass::Instance() {
  static SizeClass instance;
  return instance;
}

SizeClass::SizeClass() { Initialize(); }

void SizeClass::Initialize() {
  // 采用分段对齐策略生成 SizeClass
  // [1, 128]: 8B 对齐 -> 16 个类
  // (128, 1024]: 16B 对齐 -> 56 个类
  // (1024, 8192]: 128B 对齐 -> 56 个类 (我们用更少)
  // (8192, 32768]: 1024B 对齐

  size_t index = 0;
  size_t size = 0;

  // [8, 128]: 8B 对齐
  for (size = 8; size <= 128 && index < kNumSizeClasses; size += 8, ++index) {
    class_info_[index].size = size;
    // 批量数：小对象多取，大对象少取 (512 / size, 最少 2 个)
    class_info_[index].batch_count = std::max<size_t>(512 / size, 2);
    // Span 页数：确保每个 Span 能容纳足够多对象
    size_t bytes_needed = class_info_[index].batch_count * size * 4;
    class_info_[index].span_pages =
        std::max<size_t>(SizeToPages(bytes_needed), 1);
  }

  // (128, 1024]: 16B 对齐
  for (size = 128 + 16; size <= 1024 && index < kNumSizeClasses;
       size += 16, ++index) {
    class_info_[index].size = size;
    class_info_[index].batch_count = std::max<size_t>(512 / size, 2);
    size_t bytes_needed = class_info_[index].batch_count * size * 4;
    class_info_[index].span_pages =
        std::max<size_t>(SizeToPages(bytes_needed), 1);
  }

  // (1024, 8192]: 128B 对齐
  for (size = 1024 + 128; size <= 8192 && index < kNumSizeClasses;
       size += 128, ++index) {
    class_info_[index].size = size;
    class_info_[index].batch_count = std::max<size_t>(512 / size, 2);
    size_t bytes_needed = class_info_[index].batch_count * size * 4;
    class_info_[index].span_pages =
        std::max<size_t>(SizeToPages(bytes_needed), 1);
  }

  // (8192, 32768]: 1024B 对齐
  for (size = 8192 + 1024; size <= kMaxCacheableSize && index < kNumSizeClasses;
       size += 1024, ++index) {
    class_info_[index].size = size;
    class_info_[index].batch_count = std::max<size_t>(512 / size, 2);
    size_t bytes_needed = class_info_[index].batch_count * size * 2;
    class_info_[index].span_pages =
        std::max<size_t>(SizeToPages(bytes_needed), 1);
  }

  // 构建大小到索引的映射表
  size_t class_idx = 0;
  for (size_t s = 1; s <= kMaxCacheableSize; ++s) {
    while (class_idx < index && class_info_[class_idx].size < s) {
      ++class_idx;
    }
    if (class_idx < index) {
      class_index_[s] = class_idx;
    } else {
      class_index_[s] = index - 1;
    }
  }
  class_index_[0] = 0;
}

size_t SizeClass::GetClassIndex(size_t size) const {
  if (size == 0) {
    size = 1;
  }
  if (size > kMaxCacheableSize) {
    return kNumSizeClasses; // 表示大对象
  }
  return class_index_[size];
}

size_t SizeClass::GetClassSize(size_t class_index) const {
  if (class_index >= kNumSizeClasses) {
    return 0;
  }
  return class_info_[class_index].size;
}

size_t SizeClass::GetBatchMoveCount(size_t class_index) const {
  if (class_index >= kNumSizeClasses) {
    return 1;
  }
  return class_info_[class_index].batch_count;
}

size_t SizeClass::GetSpanPages(size_t class_index) const {
  if (class_index >= kNumSizeClasses) {
    return 1;
  }
  return class_info_[class_index].span_pages;
}

} // namespace zmalloc
