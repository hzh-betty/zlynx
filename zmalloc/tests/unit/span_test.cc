/**
 * @file span_test.cc
 * @brief Span 和 SpanList 单元测试
 */

#include <gtest/gtest.h>

#include "common.h"
#include "span.h"

namespace zmalloc {
namespace {

class SpanTest : public ::testing::Test {
protected:
  void SetUp() override {
    span_.SetPageId(100);
    span_.SetNumPages(4);
  }

  Span span_;
};

// 功能正确性测试
TEST_F(SpanTest, InitialState) {
  Span s;
  EXPECT_EQ(s.PageId(), 0);
  EXPECT_EQ(s.NumPages(), 0);
  EXPECT_EQ(s.UseCount(), 0);
  EXPECT_EQ(s.SizeClassIndex(), 0);
  EXPECT_FALSE(s.HasFreeObject());
}

TEST_F(SpanTest, PageIdAndNumPages) {
  EXPECT_EQ(span_.PageId(), 100);
  EXPECT_EQ(span_.NumPages(), 4);
}

TEST_F(SpanTest, StartAddress) {
  void *expected = reinterpret_cast<void *>(100UL << kPageShift);
  EXPECT_EQ(span_.StartAddress(), expected);
}

TEST_F(SpanTest, TotalBytes) { EXPECT_EQ(span_.TotalBytes(), 4 * kPageSize); }

TEST_F(SpanTest, UseCountOperations) {
  EXPECT_EQ(span_.UseCount(), 0);
  EXPECT_TRUE(span_.IsFullyFree());

  span_.IncrementUseCount();
  EXPECT_EQ(span_.UseCount(), 1);
  EXPECT_FALSE(span_.IsFullyFree());

  span_.IncrementUseCount();
  EXPECT_EQ(span_.UseCount(), 2);

  span_.DecrementUseCount();
  EXPECT_EQ(span_.UseCount(), 1);

  span_.DecrementUseCount();
  EXPECT_EQ(span_.UseCount(), 0);
  EXPECT_TRUE(span_.IsFullyFree());
}

TEST_F(SpanTest, SizeClassOperations) {
  EXPECT_EQ(span_.SizeClassIndex(), 0);
  span_.SetSizeClass(5);
  EXPECT_EQ(span_.SizeClassIndex(), 5);
}

TEST_F(SpanTest, FreeListOperations) {
  // 使用足够大的对象存储 next 指针 (sizeof(void*))
  void *objects[3];

  EXPECT_FALSE(span_.HasFreeObject());
  EXPECT_EQ(span_.PopFreeObject(), nullptr);

  span_.PushFreeObject(&objects[0]);
  EXPECT_TRUE(span_.HasFreeObject());

  span_.PushFreeObject(&objects[1]);
  span_.PushFreeObject(&objects[2]);

  // LIFO 顺序
  EXPECT_EQ(span_.PopFreeObject(), &objects[2]);
  EXPECT_EQ(span_.PopFreeObject(), &objects[1]);
  EXPECT_EQ(span_.PopFreeObject(), &objects[0]);
  EXPECT_FALSE(span_.HasFreeObject());
}

// SpanList 测试
class SpanListTest : public ::testing::Test {
protected:
  SpanList list_;
  Span spans_[5];
};

TEST_F(SpanListTest, InitiallyEmpty) {
  EXPECT_TRUE(list_.IsEmpty());
  EXPECT_EQ(list_.Front(), nullptr);
}

TEST_F(SpanListTest, PushFront) {
  list_.PushFront(&spans_[0]);
  EXPECT_FALSE(list_.IsEmpty());
  EXPECT_EQ(list_.Front(), &spans_[0]);

  list_.PushFront(&spans_[1]);
  EXPECT_EQ(list_.Front(), &spans_[1]);
}

TEST_F(SpanListTest, PopFront) {
  list_.PushFront(&spans_[0]);
  list_.PushFront(&spans_[1]);
  list_.PushFront(&spans_[2]);

  EXPECT_EQ(list_.PopFront(), &spans_[2]);
  EXPECT_EQ(list_.PopFront(), &spans_[1]);
  EXPECT_EQ(list_.PopFront(), &spans_[0]);
  EXPECT_TRUE(list_.IsEmpty());
  EXPECT_EQ(list_.PopFront(), nullptr);
}

TEST_F(SpanListTest, Erase) {
  list_.PushFront(&spans_[0]);
  list_.PushFront(&spans_[1]);
  list_.PushFront(&spans_[2]);

  // 删除中间元素
  list_.Erase(&spans_[1]);
  EXPECT_EQ(list_.PopFront(), &spans_[2]);
  EXPECT_EQ(list_.PopFront(), &spans_[0]);
  EXPECT_TRUE(list_.IsEmpty());
}

TEST_F(SpanListTest, EraseFirst) {
  list_.PushFront(&spans_[0]);
  list_.PushFront(&spans_[1]);

  list_.Erase(&spans_[1]);
  EXPECT_EQ(list_.Front(), &spans_[0]);
}

TEST_F(SpanListTest, EraseLast) {
  list_.PushFront(&spans_[0]);
  list_.PushFront(&spans_[1]);

  list_.Erase(&spans_[0]);
  EXPECT_EQ(list_.Front(), &spans_[1]);
  EXPECT_EQ(list_.PopFront(), &spans_[1]);
  EXPECT_TRUE(list_.IsEmpty());
}

// 边界测试
TEST_F(SpanTest, LargePageId) {
  span_.SetPageId(0xFFFFFFFF);
  EXPECT_EQ(span_.PageId(), 0xFFFFFFFF);
}

TEST_F(SpanTest, ZeroPages) {
  Span s;
  s.SetNumPages(0);
  EXPECT_EQ(s.TotalBytes(), 0);
}

} // namespace
} // namespace zmalloc
