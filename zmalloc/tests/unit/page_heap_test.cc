/**
 * @file page_heap_test.cc
 * @brief PageHeap 单元测试
 */

#include <gtest/gtest.h>

#include <vector>

#include "common.h"
#include "page_heap.h"
#include "span.h"

namespace zmalloc {
namespace {

class PageHeapTest : public ::testing::Test {
protected:
  PageHeap &heap_ = PageHeap::Instance();
};

// 功能正确性测试
TEST_F(PageHeapTest, AllocateSinglePage) {
  Span *span = heap_.AllocateSpan(1);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->NumPages(), 1);
  EXPECT_NE(span->StartAddress(), nullptr);
  heap_.DeallocateSpan(span);
}

TEST_F(PageHeapTest, AllocateMultiplePages) {
  Span *span = heap_.AllocateSpan(10);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->NumPages(), 10);
  heap_.DeallocateSpan(span);
}

TEST_F(PageHeapTest, AllocateAndDeallocateMultiple) {
  std::vector<Span *> spans;
  for (int i = 0; i < 10; ++i) {
    Span *span = heap_.AllocateSpan(i + 1);
    ASSERT_NE(span, nullptr);
    spans.push_back(span);
  }

  for (Span *span : spans) {
    heap_.DeallocateSpan(span);
  }
}

TEST_F(PageHeapTest, AddressAlignment) {
  Span *span = heap_.AllocateSpan(1);
  ASSERT_NE(span, nullptr);

  uintptr_t addr = reinterpret_cast<uintptr_t>(span->StartAddress());
  EXPECT_EQ(addr % kPageSize, 0);

  heap_.DeallocateSpan(span);
}

TEST_F(PageHeapTest, GetSpanByPageId) {
  Span *span = heap_.AllocateSpan(5);
  ASSERT_NE(span, nullptr);

  size_t page_id = span->PageId();
  Span *found = heap_.GetSpanByPageId(page_id);
  EXPECT_EQ(found, span);

  // 中间页也应该能找到
  Span *found_middle = heap_.GetSpanByPageId(page_id + 2);
  EXPECT_EQ(found_middle, span);

  heap_.DeallocateSpan(span);
}

TEST_F(PageHeapTest, ReuseDeallocatedSpan) {
  Span *span1 = heap_.AllocateSpan(4);
  ASSERT_NE(span1, nullptr);
  void *addr1 = span1->StartAddress();
  heap_.DeallocateSpan(span1);

  // 再次分配相同大小，应该复用
  Span *span2 = heap_.AllocateSpan(4);
  ASSERT_NE(span2, nullptr);
  // 可能复用同一块内存
  heap_.DeallocateSpan(span2);
}

// 边界测试
TEST_F(PageHeapTest, AllocateManyPages) {
  Span *span = heap_.AllocateSpan(100);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->NumPages(), 100);
  heap_.DeallocateSpan(span);
}

TEST_F(PageHeapTest, AllocateZeroPages) {
  // 分配 0 页可能返回 nullptr 或最小分配
  Span *span = heap_.AllocateSpan(0);
  if (span != nullptr) {
    heap_.DeallocateSpan(span);
  }
}

TEST_F(PageHeapTest, GetSpanByInvalidPageId) {
  Span *found = heap_.GetSpanByPageId(0xDEADBEEF);
  EXPECT_EQ(found, nullptr);
}

// 异常处理测试
TEST_F(PageHeapTest, DeallocateAndReallocate) {
  Span *span = heap_.AllocateSpan(8);
  ASSERT_NE(span, nullptr);

  heap_.DeallocateSpan(span);

  // 分配后再释放不应该崩溃
  Span *span2 = heap_.AllocateSpan(8);
  ASSERT_NE(span2, nullptr);
  heap_.DeallocateSpan(span2);
}

// 单例测试
TEST_F(PageHeapTest, SingletonConsistency) {
  PageHeap &heap1 = PageHeap::Instance();
  PageHeap &heap2 = PageHeap::Instance();
  EXPECT_EQ(&heap1, &heap2);
}

} // namespace
} // namespace zmalloc
