/**
 * @file size_class.h
 * @brief 大小分类管理
 *
 * 将不同大小的内存请求映射到固定的 SizeClass，减少内存碎片。
 */

#ifndef ZMALLOC_SIZE_CLASS_H_
#define ZMALLOC_SIZE_CLASS_H_

#include "common.h"

namespace zmalloc {

/**
 * @class SizeClass
 * @brief 大小分类映射表
 *
 * 采用分段对齐策略：
 * - [1, 128] bytes: 8B 对齐
 * - (128, 1024] bytes: 16B 对齐
 * - (1024, 8192] bytes: 128B 对齐
 * - (8192, 32768] bytes: 1024B 对齐
 */
class SizeClass {
public:
  /// 获取单例实例
  static SizeClass &Instance();

  /// 根据请求大小获取 SizeClass 索引
  size_t GetClassIndex(size_t size) const;

  /// 获取指定 SizeClass 的实际大小
  size_t GetClassSize(size_t class_index) const;

  /// 获取指定 SizeClass 每次批量获取的对象数
  size_t GetBatchMoveCount(size_t class_index) const;

  /// 获取指定 SizeClass 每个 Span 包含的页数
  size_t GetSpanPages(size_t class_index) const;

private:
  SizeClass();

  /// 初始化大小分类表
  void Initialize();

  struct ClassInfo {
    size_t size;        ///< 对齐后的大小
    size_t batch_count; ///< 批量获取数量
    size_t span_pages;  ///< 每个 Span 的页数
  };

  ClassInfo class_info_[kNumSizeClasses];
  size_t class_index_[kMaxCacheableSize + 1]; ///< 大小到索引的映射
};

/// 便捷函数
inline size_t GetSizeClass(size_t size) {
  return SizeClass::Instance().GetClassIndex(size);
}

inline size_t GetClassSize(size_t class_index) {
  return SizeClass::Instance().GetClassSize(class_index);
}

} // namespace zmalloc

#endif // ZMALLOC_SIZE_CLASS_H_
