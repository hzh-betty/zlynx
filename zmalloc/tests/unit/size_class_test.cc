/**
 * @file size_class_test.cc
 * @brief SizeClass 单元测试
 */

#include "common.h"
#include <gtest/gtest.h>

namespace zmalloc {
namespace {

// 对齐测试
class SizeClassRoundUpTest : public ::testing::Test {};

TEST_F(SizeClassRoundUpTest, SmallSizes) {
  // [1, 128] 8 字节对齐
  EXPECT_EQ(SizeClass::round_up(1), 8);
  EXPECT_EQ(SizeClass::round_up(7), 8);
  EXPECT_EQ(SizeClass::round_up(8), 8);
  EXPECT_EQ(SizeClass::round_up(9), 16);
  EXPECT_EQ(SizeClass::round_up(128), 128);
}

TEST_F(SizeClassRoundUpTest, MediumSizes) {
  // [129, 1024] 16 字节对齐
  EXPECT_EQ(SizeClass::round_up(129), 144);
  EXPECT_EQ(SizeClass::round_up(256), 256);
  EXPECT_EQ(SizeClass::round_up(1024), 1024);
}

TEST_F(SizeClassRoundUpTest, LargeSizes) {
  // [1025, 8KB] 128 字节对齐
  EXPECT_EQ(SizeClass::round_up(1025), 1152);
  EXPECT_EQ(SizeClass::round_up(8 * 1024), 8 * 1024);
}

TEST_F(SizeClassRoundUpTest, VeryLargeSizes) {
  // [8KB+1, 64KB] 1KB 对齐
  EXPECT_EQ(SizeClass::round_up(8 * 1024 + 1), 9 * 1024);
  EXPECT_EQ(SizeClass::round_up(64 * 1024), 64 * 1024);
}

TEST_F(SizeClassRoundUpTest, HugeSizes) {
  // [64KB+1, 256KB] 8KB 对齐
  EXPECT_EQ(SizeClass::round_up(64 * 1024 + 1), 72 * 1024);
  EXPECT_EQ(SizeClass::round_up(256 * 1024), 256 * 1024);
}

// 索引测试
class SizeClassIndexTest : public ::testing::Test {};

TEST_F(SizeClassIndexTest, SmallSizesIndex) {
  EXPECT_EQ(SizeClass::index(1), 0);
  EXPECT_EQ(SizeClass::index(8), 0);
  EXPECT_EQ(SizeClass::index(9), 1);
  EXPECT_EQ(SizeClass::index(128), 15);
}

TEST_F(SizeClassIndexTest, MediumSizesIndex) {
  EXPECT_EQ(SizeClass::index(129), 16);
  EXPECT_EQ(SizeClass::index(1024), 71);
}

TEST_F(SizeClassIndexTest, LargeSizesIndex) {
  EXPECT_EQ(SizeClass::index(1025), 72);
  EXPECT_EQ(SizeClass::index(8 * 1024), 127);
}

TEST_F(SizeClassIndexTest, NumMoveSize) {
  // 优化后算法：目标 4KB 单次传输，最多 128 个对象
  // 小对象 (8字节): 4096/8 = 512，但上限 128
  EXPECT_EQ(SizeClass::num_move_size(8), 128);
  // 大对象上限低
  EXPECT_GE(SizeClass::num_move_size(256 * 1024), 2);
}

TEST_F(SizeClassIndexTest, NumMovePage) {
  EXPECT_GE(SizeClass::num_move_page(8), 1);
  EXPECT_GE(SizeClass::num_move_page(8 * 1024), 1);
}

// 更多 round_up 测试
TEST_F(SizeClassRoundUpTest, AllBoundaries) {
  // 边界值测试
  EXPECT_EQ(SizeClass::round_up(1), 8);
  EXPECT_EQ(SizeClass::round_up(127), 128);
  EXPECT_EQ(SizeClass::round_up(1023), 1024);
  EXPECT_EQ(SizeClass::round_up(8191), 8192);
}

TEST_F(SizeClassRoundUpTest, ExactMultiples) {
  // 已对齐的值应该不变
  EXPECT_EQ(SizeClass::round_up(8), 8);
  EXPECT_EQ(SizeClass::round_up(16), 16);
  EXPECT_EQ(SizeClass::round_up(64), 64);
  EXPECT_EQ(SizeClass::round_up(256), 256);
  EXPECT_EQ(SizeClass::round_up(1024), 1024);
  EXPECT_EQ(SizeClass::round_up(8 * 1024), 8 * 1024);
}

TEST_F(SizeClassRoundUpTest, OneByteOverBoundary) {
  // 边界+1
  EXPECT_EQ(SizeClass::round_up(9), 16);
  EXPECT_EQ(SizeClass::round_up(17), 24);
  EXPECT_EQ(SizeClass::round_up(65), 72);
  EXPECT_EQ(SizeClass::round_up(130), 144);
  EXPECT_EQ(SizeClass::round_up(1026), 1152);
}

// 更多 index 测试
TEST_F(SizeClassIndexTest, VeryLargeSizesIndex) {
  EXPECT_EQ(SizeClass::index(8 * 1024 + 1), 128);
  EXPECT_EQ(SizeClass::index(64 * 1024), 183);
}

TEST_F(SizeClassIndexTest, HugeSizesIndex) {
  EXPECT_EQ(SizeClass::index(64 * 1024 + 1), 184);
  EXPECT_EQ(SizeClass::index(256 * 1024), 207);
}

TEST_F(SizeClassIndexTest, IndexMonotonicity) {
  // 索引应该是单调递增的
  size_t prev_idx = 0;
  for (size_t size = 1; size <= 256 * 1024; size += 127) {
    size_t idx = SizeClass::index(size);
    EXPECT_GE(idx, prev_idx);
    prev_idx = idx;
  }
}

// num_move_size 更多测试
TEST_F(SizeClassIndexTest, NumMoveSizeSmall) {
  // 小对象应该批量多
  EXPECT_EQ(SizeClass::num_move_size(16), 128);
  EXPECT_EQ(SizeClass::num_move_size(32), 128);
  EXPECT_EQ(SizeClass::num_move_size(64), 64);
}

TEST_F(SizeClassIndexTest, NumMoveSizeMedium) {
  // 中等对象
  EXPECT_EQ(SizeClass::num_move_size(128), 32);
  EXPECT_EQ(SizeClass::num_move_size(256), 16);
  EXPECT_EQ(SizeClass::num_move_size(512), 8);
}

TEST_F(SizeClassIndexTest, NumMoveSizeLarge) {
  // 大对象批量少
  EXPECT_EQ(SizeClass::num_move_size(1024), 4);
  EXPECT_EQ(SizeClass::num_move_size(2048), 2);
  EXPECT_EQ(SizeClass::num_move_size(4096), 2);
}

TEST_F(SizeClassIndexTest, NumMoveSizeMinBound) {
  // 最小下限是 2
  EXPECT_GE(SizeClass::num_move_size(128 * 1024), 2);
  EXPECT_GE(SizeClass::num_move_size(256 * 1024), 2);
}

TEST_F(SizeClassIndexTest, NumMoveSizeMaxBound) {
  // 最大上限是 128
  EXPECT_LE(SizeClass::num_move_size(1), 128);
  EXPECT_LE(SizeClass::num_move_size(8), 128);
}

// num_move_page 更多测试
TEST_F(SizeClassIndexTest, NumMovePageSmall) {
  EXPECT_GE(SizeClass::num_move_page(1), 1);
  EXPECT_GE(SizeClass::num_move_page(16), 1);
}

TEST_F(SizeClassIndexTest, NumMovePageLarge) {
  EXPECT_GE(SizeClass::num_move_page(64 * 1024), 1);
  EXPECT_GE(SizeClass::num_move_page(256 * 1024), 1);
}

// round_up 与 index 一致性
TEST(SizeClassConsistencyTest, RoundUpIndexConsistency) {
  for (size_t size = 1; size <= 256 * 1024; size += 100) {
    size_t rounded = SizeClass::round_up(size);
    EXPECT_GE(rounded, size);
    EXPECT_LE(rounded, 256 * 1024);
  }
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
