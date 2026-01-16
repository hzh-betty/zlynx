/**
 * @file size_class_test.cc
 * @brief SizeClass 单元测试
 */

#include <gtest/gtest.h>

#include "common.h"
#include "size_class.h"

namespace zmalloc {
namespace {

class SizeClassTest : public ::testing::Test {
protected:
  SizeClass &sc_ = SizeClass::Instance();
};

// 功能正确性测试
TEST_F(SizeClassTest, ZeroSizeReturnsFirstClass) {
  EXPECT_EQ(sc_.GetClassIndex(0), 0);
}

TEST_F(SizeClassTest, SmallSizesRoundUpToAlignment) {
  // 1-8 字节应该映射到 8 字节类
  for (size_t i = 1; i <= 8; ++i) {
    EXPECT_EQ(sc_.GetClassSize(sc_.GetClassIndex(i)), 8);
  }

  // 9-16 字节应该映射到 16 字节类
  for (size_t i = 9; i <= 16; ++i) {
    EXPECT_EQ(sc_.GetClassSize(sc_.GetClassIndex(i)), 16);
  }
}

TEST_F(SizeClassTest, LargeSizesReturnLargeClass) {
  // 超过 kMaxCacheableSize 返回 kNumSizeClasses
  EXPECT_EQ(sc_.GetClassIndex(kMaxCacheableSize + 1), kNumSizeClasses);
  EXPECT_EQ(sc_.GetClassIndex(100000), kNumSizeClasses);
}

TEST_F(SizeClassTest, ClassSizeIncreases) {
  size_t prev_size = 0;
  for (size_t i = 0; i < kNumSizeClasses; ++i) {
    size_t class_size = sc_.GetClassSize(i);
    if (class_size > 0) {
      EXPECT_GE(class_size, prev_size);
      prev_size = class_size;
    }
  }
}

TEST_F(SizeClassTest, ClassSizeAlwaysGreaterOrEqualToRequest) {
  for (size_t size = 1; size <= kMaxCacheableSize; ++size) {
    size_t class_index = sc_.GetClassIndex(size);
    size_t class_size = sc_.GetClassSize(class_index);
    EXPECT_GE(class_size, size) << "Failed for size: " << size;
  }
}

TEST_F(SizeClassTest, BatchCountIsReasonable) {
  for (size_t i = 0; i < kNumSizeClasses; ++i) {
    size_t class_size = sc_.GetClassSize(i);
    if (class_size == 0)
      continue; // 跳过未初始化的类
    size_t batch = sc_.GetBatchMoveCount(i);
    EXPECT_GE(batch, 2); // 至少批量获取 2 个
  }
}

TEST_F(SizeClassTest, SpanPagesIsPositive) {
  for (size_t i = 0; i < kNumSizeClasses; ++i) {
    size_t class_size = sc_.GetClassSize(i);
    if (class_size == 0)
      continue; // 跳过未初始化的类
    size_t pages = sc_.GetSpanPages(i);
    EXPECT_GE(pages, 1);
  }
}

TEST_F(SizeClassTest, ExactAlignmentBoundaries) {
  // 测试对齐边界
  EXPECT_EQ(sc_.GetClassSize(sc_.GetClassIndex(128)), 128);
  EXPECT_EQ(sc_.GetClassSize(sc_.GetClassIndex(1024)), 1024);
}

TEST_F(SizeClassTest, InvalidClassIndexReturnsZero) {
  EXPECT_EQ(sc_.GetClassSize(kNumSizeClasses), 0);
  EXPECT_EQ(sc_.GetClassSize(kNumSizeClasses + 1), 0);
}

// 边界测试
TEST_F(SizeClassTest, MaxCacheableSizeHandled) {
  size_t class_index = sc_.GetClassIndex(kMaxCacheableSize);
  EXPECT_LT(class_index, kNumSizeClasses);
  EXPECT_GE(sc_.GetClassSize(class_index), kMaxCacheableSize);
}

TEST_F(SizeClassTest, SizeOfOne) {
  size_t class_index = sc_.GetClassIndex(1);
  EXPECT_EQ(class_index, 0);
  EXPECT_EQ(sc_.GetClassSize(class_index), 8);
}

TEST_F(SizeClassTest, SizeClassCoverage) {
  // 确保没有大小区间被遗漏
  size_t prev_max = 0;
  for (size_t i = 0; i < kNumSizeClasses; ++i) {
    size_t class_size = sc_.GetClassSize(i);
    if (class_size > 0) {
      // 检查从 prev_max+1 到 class_size 的所有大小都映射到这个类或之前的类
      for (size_t s = prev_max + 1; s <= class_size; ++s) {
        size_t idx = sc_.GetClassIndex(s);
        EXPECT_LE(idx, i) << "Size " << s << " mapped to class " << idx
                          << " but expected <= " << i;
      }
      prev_max = class_size;
    }
  }
}

// 状态测试
TEST_F(SizeClassTest, SingletonConsistency) {
  SizeClass &sc1 = SizeClass::Instance();
  SizeClass &sc2 = SizeClass::Instance();
  EXPECT_EQ(&sc1, &sc2);
}

} // namespace
} // namespace zmalloc
