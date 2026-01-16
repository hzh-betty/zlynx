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

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
