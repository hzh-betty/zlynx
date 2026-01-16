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

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
