/**
 * @file free_list_test.cc
 * @brief FreeList 单元测试
 */

#include "common.h"
#include <gtest/gtest.h>

namespace zmalloc {
namespace {

class FreeListTest : public ::testing::Test {
protected:
  FreeList list_;
  // 用于测试的内存块
  char blocks_[10][sizeof(void *)];

  void SetUp() override {
    // 初始化
    for (int i = 0; i < 10; ++i) {
      next_obj(blocks_[i]) = nullptr;
    }
  }
};

TEST_F(FreeListTest, InitiallyEmpty) {
  EXPECT_TRUE(list_.empty());
  EXPECT_EQ(list_.size(), 0);
}

TEST_F(FreeListTest, PushAndPop) {
  list_.push(blocks_[0]);
  EXPECT_FALSE(list_.empty());
  EXPECT_EQ(list_.size(), 1);

  void *obj = list_.pop();
  EXPECT_EQ(obj, blocks_[0]);
  EXPECT_TRUE(list_.empty());
  EXPECT_EQ(list_.size(), 0);
}

TEST_F(FreeListTest, MultiplePushPop) {
  list_.push(blocks_[0]);
  list_.push(blocks_[1]);
  list_.push(blocks_[2]);
  EXPECT_EQ(list_.size(), 3);

  // LIFO 顺序
  EXPECT_EQ(list_.pop(), blocks_[2]);
  EXPECT_EQ(list_.pop(), blocks_[1]);
  EXPECT_EQ(list_.pop(), blocks_[0]);
  EXPECT_TRUE(list_.empty());
}

TEST_F(FreeListTest, PushRange) {
  // 构建一个链表
  next_obj(blocks_[0]) = blocks_[1];
  next_obj(blocks_[1]) = blocks_[2];
  next_obj(blocks_[2]) = nullptr;

  list_.push_range(blocks_[0], blocks_[2], 3);
  EXPECT_EQ(list_.size(), 3);
  EXPECT_FALSE(list_.empty());
}

TEST_F(FreeListTest, PopRange) {
  list_.push(blocks_[0]);
  list_.push(blocks_[1]);
  list_.push(blocks_[2]);
  list_.push(blocks_[3]);
  list_.push(blocks_[4]);

  void *start = nullptr;
  void *end = nullptr;
  list_.pop_range(start, end, 3);

  EXPECT_EQ(list_.size(), 2);
  EXPECT_NE(start, nullptr);
  EXPECT_NE(end, nullptr);
}

TEST_F(FreeListTest, MaxSize) {
  EXPECT_EQ(list_.max_size(), 1);
  list_.max_size() = 10;
  EXPECT_EQ(list_.max_size(), 10);
}

// 边界条件：只有一个元素
TEST_F(FreeListTest, PopRangeSingleElement) {
  list_.push(blocks_[0]);
  void *start = nullptr;
  void *end = nullptr;
  list_.pop_range(start, end, 1);
  EXPECT_EQ(start, blocks_[0]);
  EXPECT_EQ(end, blocks_[0]);
  EXPECT_EQ(list_.size(), 0);
}

// 边界条件：pop_range 请求恰好等于可用数量
TEST_F(FreeListTest, PopRangeExactCount) {
  list_.push(blocks_[0]);
  list_.push(blocks_[1]);
  void *start = nullptr;
  void *end = nullptr;
  list_.pop_range(start, end, 2);
  // 取出全部
  EXPECT_EQ(list_.size(), 0);
  EXPECT_NE(start, nullptr);
  EXPECT_NE(end, nullptr);
}

// 大量 Push 测试
TEST_F(FreeListTest, ManyPush) {
  char large_blocks[100][sizeof(void *)];
  for (int i = 0; i < 100; ++i) {
    next_obj(large_blocks[i]) = nullptr;
    list_.push(large_blocks[i]);
  }
  EXPECT_EQ(list_.size(), 100);
  for (int i = 0; i < 100; ++i) {
    EXPECT_NE(list_.pop(), nullptr);
  }
  EXPECT_TRUE(list_.empty());
}

// Push/Pop 交替
TEST_F(FreeListTest, AlternatePushPop) {
  for (int i = 0; i < 50; ++i) {
    list_.push(blocks_[0]);
    EXPECT_EQ(list_.pop(), blocks_[0]);
  }
  EXPECT_TRUE(list_.empty());
}

// 验证 LIFO 顺序
TEST_F(FreeListTest, LIFOOrder) {
  for (int i = 0; i < 10; ++i) {
    list_.push(blocks_[i]);
  }
  for (int i = 9; i >= 0; --i) {
    EXPECT_EQ(list_.pop(), blocks_[i]);
  }
}

// push_range 后验证大小
TEST_F(FreeListTest, PushRangeSize) {
  next_obj(blocks_[0]) = blocks_[1];
  next_obj(blocks_[1]) = blocks_[2];
  next_obj(blocks_[2]) = blocks_[3];
  next_obj(blocks_[3]) = blocks_[4];
  next_obj(blocks_[4]) = nullptr;

  list_.push_range(blocks_[0], blocks_[4], 5);
  EXPECT_EQ(list_.size(), 5);
}

// empty() 和 size() 一致性
TEST_F(FreeListTest, EmptySizeConsistency) {
  EXPECT_TRUE(list_.empty());
  EXPECT_EQ(list_.size(), 0);

  list_.push(blocks_[0]);
  EXPECT_FALSE(list_.empty());
  EXPECT_EQ(list_.size(), 1);

  list_.pop();
  EXPECT_TRUE(list_.empty());
  EXPECT_EQ(list_.size(), 0);
}

// max_size 修改测试
TEST_F(FreeListTest, MaxSizeModification) {
  list_.max_size() = 5;
  EXPECT_EQ(list_.max_size(), 5);
  list_.max_size() += 3;
  EXPECT_EQ(list_.max_size(), 8);
}

// 连续 push_range
TEST_F(FreeListTest, MultiplePushRange) {
  next_obj(blocks_[0]) = blocks_[1];
  next_obj(blocks_[1]) = nullptr;
  list_.push_range(blocks_[0], blocks_[1], 2);

  next_obj(blocks_[2]) = blocks_[3];
  next_obj(blocks_[3]) = nullptr;
  list_.push_range(blocks_[2], blocks_[3], 2);

  EXPECT_EQ(list_.size(), 4);
}

// pop_range 验证链表结构
TEST_F(FreeListTest, PopRangeChainStructure) {
  for (int i = 0; i < 5; ++i) {
    list_.push(blocks_[i]);
  }
  void *start = nullptr;
  void *end = nullptr;
  list_.pop_range(start, end, 3);

  // 验证取出的是一个链表
  EXPECT_NE(start, nullptr);
  EXPECT_NE(end, nullptr);
  // 验证剩余
  EXPECT_EQ(list_.size(), 2);
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
