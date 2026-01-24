/**
 * @file stealable_queue_bitmap_test.cc
 * @brief StealableQueueBitmap 单元测试
 */

#include "stealable_queue_bitmap.h"
#include "zcoroutine_logger.h"

#include <gtest/gtest.h>


using namespace zcoroutine;

class StealableQueueBitmapTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  }
};

TEST_F(StealableQueueBitmapTest, WorkerCountLessOrEqualOneAlwaysReturnsMinusOne) {
  {
    StealableQueueBitmap bitmap(0);
    EXPECT_EQ(bitmap.find_victim(0), -1);
    EXPECT_EQ(bitmap.find_victim(-1), -1);
  }
  {
    StealableQueueBitmap bitmap(1);
    bitmap.set(0);
    EXPECT_EQ(bitmap.find_victim(0), -1);
    EXPECT_EQ(bitmap.find_victim(-1), -1);
    EXPECT_EQ(bitmap.find_victim(1), -1);
  }
}

TEST_F(StealableQueueBitmapTest, SetClearOutOfRangeNoEffect) {
  StealableQueueBitmap bitmap(4);

  bitmap.set(100);
  bitmap.clear(100);
  EXPECT_EQ(bitmap.find_victim(0), -1);

  bitmap.set(2);
  EXPECT_EQ(bitmap.find_victim(0), 2);

  bitmap.clear(2);
  EXPECT_EQ(bitmap.find_victim(0), -1);
}

TEST_F(StealableQueueBitmapTest, TestAndAnyReflectSetClear) {
  StealableQueueBitmap bitmap(4);
  EXPECT_FALSE(bitmap.any());
  EXPECT_FALSE(bitmap.test(0));

  bitmap.set(2);
  EXPECT_TRUE(bitmap.any());
  EXPECT_TRUE(bitmap.test(2));
  EXPECT_FALSE(bitmap.test(1));

  bitmap.clear(2);
  EXPECT_FALSE(bitmap.any());
  EXPECT_FALSE(bitmap.test(2));
}

TEST_F(StealableQueueBitmapTest, FindNonStealablePrefersZeroBits) {
  StealableQueueBitmap bitmap(4);

  // 初始全 0：从 0 开始应返回 0
  EXPECT_EQ(bitmap.find_non_stealable(0), 0);

  // 将 0 和 1 设为 1，则从 0 开始应找到 2
  bitmap.set(0);
  bitmap.set(1);
  EXPECT_EQ(bitmap.find_non_stealable(0), 2);

  // 从 3 开始扫描：3 为 0，应返回 3
  EXPECT_EQ(bitmap.find_non_stealable(3), 3);

  // 全设为 1：找不到 0
  bitmap.set(2);
  bitmap.set(3);
  EXPECT_EQ(bitmap.find_non_stealable(0), -1);
}

TEST_F(StealableQueueBitmapTest, ExcludesSelfFromVictimSelection) {
  StealableQueueBitmap bitmap(8);

  bitmap.set(3);
  EXPECT_EQ(bitmap.find_victim(3), -1);

  bitmap.set(4);
  // self=3 -> start=4，应选到4
  EXPECT_EQ(bitmap.find_victim(3), 4);

  // self=4 -> start=5，环回后会看到3（4被排除）
  EXPECT_EQ(bitmap.find_victim(4), 3);
}

TEST_F(StealableQueueBitmapTest, ScanOrderWrapAroundFromSelfPlusOne) {
  StealableQueueBitmap bitmap(6);
  bitmap.set(0);
  bitmap.set(4);

  // self=0 -> start=1，先看到4
  EXPECT_EQ(bitmap.find_victim(0), 4);

  // self=4 -> start=5，先扫[5,6)无，再环回扫[0,5)看到0
  EXPECT_EQ(bitmap.find_victim(4), 0);

  // self=5 -> start=0，直接看到0
  EXPECT_EQ(bitmap.find_victim(5), 0);
}

TEST_F(StealableQueueBitmapTest, MultiWordBitmapHandlesIdsAbove64) {
  // 130 bits -> 3 words
  StealableQueueBitmap bitmap(130);

  bitmap.set(65);
  bitmap.set(129);

  // self=64 -> start=65，应命中65
  EXPECT_EQ(bitmap.find_victim(64), 65);

  // self=128 -> start=129，应命中129
  EXPECT_EQ(bitmap.find_victim(128), 129);

  // self=129 被排除，应环回命中65
  EXPECT_EQ(bitmap.find_victim(129), 65);
}

TEST_F(StealableQueueBitmapTest, InvalidSelfIdReturnsMinusOne) {
  StealableQueueBitmap bitmap(8);
  bitmap.set(2);

  EXPECT_EQ(bitmap.find_victim(-1), -1);
  EXPECT_EQ(bitmap.find_victim(8), -1);
}

TEST_F(StealableQueueBitmapTest, MultipleCandidatesPickLowestInScanOrder) {
  StealableQueueBitmap bitmap(16);
  bitmap.set(5);
  bitmap.set(7);
  bitmap.set(12);

  // self=3 -> start=4，应先命中5
  EXPECT_EQ(bitmap.find_victim(3), 5);

  // self=5 -> start=6，应先命中7（5被排除）
  EXPECT_EQ(bitmap.find_victim(5), 7);

  // self=7 -> start=8，应先命中12
  EXPECT_EQ(bitmap.find_victim(7), 12);

  // self=12 -> start=13，环回后命中5
  EXPECT_EQ(bitmap.find_victim(12), 5);
}

TEST_F(StealableQueueBitmapTest, SetThenClearRemovesVictim) {
  StealableQueueBitmap bitmap(8);

  bitmap.set(6);
  EXPECT_EQ(bitmap.find_victim(0), 6);

  bitmap.clear(6);
  EXPECT_EQ(bitmap.find_victim(0), -1);
}

TEST_F(StealableQueueBitmapTest, AllWorkersSetReturnsNextWorkerDeterministically) {
  constexpr size_t kN = 128;
  StealableQueueBitmap bitmap(kN);
  for (size_t i = 0; i < kN; ++i) {
    bitmap.set(i);
  }

  for (size_t self = 0; self < kN; ++self) {
    const int victim = bitmap.find_victim(static_cast<int>(self));
    ASSERT_GE(victim, 0);
    ASSERT_LT(static_cast<size_t>(victim), kN);
    EXPECT_NE(static_cast<size_t>(victim), self);
    EXPECT_EQ(static_cast<size_t>(victim), (self + 1) % kN);
  }
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
