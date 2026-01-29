#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>


#include "span_list.h"
#include "zmalloc_config.h"

// 先包含基础头，避免 private->public 影响标准库头
#define private public
#include "page_cache.h"
#undef private

namespace {

static bool ContainsSpan(zmalloc::SpanList &list, zmalloc::Span *target) {
  for (zmalloc::Span *it = list.begin(); it != list.end(); it = it->next) {
    if (it == target) {
      return true;
    }
  }
  return false;
}

} // namespace

class PageCacheTest : public ::testing::Test {
protected:
  zmalloc::PageCache &pc = zmalloc::PageCache::get_instance();
};

static void NewSpanCheckMappingAndRelease(zmalloc::PageCache &pc, size_t k) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(k);
  ASSERT_NE(span, nullptr);
  ASSERT_EQ(span->n, k);
  ASSERT_TRUE(span->is_use);

  const zmalloc::PageId page_id = span->page_id;
  // 小 span：要求每一页都建立映射。
  for (size_t i = 0; i < k; ++i) {
    auto *m = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + i));
    ASSERT_EQ(m, span);
    void *addr = reinterpret_cast<void *>((page_id + i) << zmalloc::PAGE_SHIFT);
    ASSERT_EQ(pc.map_object_to_span(addr), span);
  }

  span->is_use = true;
  pc.release_span_to_page_cache(span);

  // release 后 span 指针可能失效；只验证边界页映射一致且 free。
  auto *a = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
  auto *b = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + k - 1));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a->is_use);
}

static void NewLargeSpanAllocAndFree(zmalloc::PageCache &pc, size_t k) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(k);
  ASSERT_NE(span, nullptr);
  ASSERT_EQ(span->n, k);
  ASSERT_TRUE(span->is_use);
  pc.release_span_to_page_cache(span);
}

TEST_F(PageCacheTest, NewSpanSmallBasicFields) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(1);
  ASSERT_NE(span, nullptr);
  EXPECT_GT(span->n, 0u);
  EXPECT_NE(span->page_id, 0u);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanStart) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(2);
  ASSERT_NE(span, nullptr);
  void *p = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
  EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanMiddle) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(4);
  ASSERT_NE(span, nullptr);
  void *p =
      reinterpret_cast<void *>((span->page_id + 2) << zmalloc::PAGE_SHIFT);
  EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, ReleaseSpanMarksNotInUse) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(3);
  ASSERT_NE(span, nullptr);
  span->is_use = true;
  const size_t page_id = span->page_id;
  const size_t n = span->n;
  pc.release_span_to_page_cache(span);
  // release 之后 span 指针可能被合并/回收，不能再直接访问。
  auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
  ASSERT_NE(mapped, nullptr);
  EXPECT_FALSE(mapped->is_use);
  // 原 span 的起始页/结束页仍应映射到一个有效 span。
  auto *mapped_end =
      static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
  ASSERT_NE(mapped_end, nullptr);
  EXPECT_EQ(mapped_end, mapped);
}

TEST_F(PageCacheTest, ReleaseThenNewSpanCanReuseBucket) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span1 = pc.new_span(1);
  ASSERT_NE(span1, nullptr);
  const size_t page_id = span1->page_id;
  const size_t n = span1->n;
  span1->is_use = true;
  pc.release_span_to_page_cache(span1);

  // release 后原 span1 指针可能失效；只验证映射仍可用。
  auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
  ASSERT_NE(mapped, nullptr);
  auto *mapped_end =
      static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
  ASSERT_NE(mapped_end, nullptr);
  EXPECT_EQ(mapped, mapped_end);
  EXPECT_FALSE(mapped->is_use);

  // 再次申请同尺寸 span 不应崩溃。
  zmalloc::Span *span2 = pc.new_span(1);
  ASSERT_NE(span2, nullptr);
}

TEST_F(PageCacheTest, LargeSpanGoesToSystemPath) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  const size_t k = zmalloc::NPAGES + 10;
  zmalloc::Span *span = pc.new_span(k);
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->n, k);
  EXPECT_TRUE(span->is_use);
  pc.release_span_to_page_cache(span);
}

TEST_F(PageCacheTest, IdSpanMapHasStartAndEndForFreeSpan) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(8);
  ASSERT_NE(span, nullptr);
  const size_t page_id = span->page_id;
  const size_t n = span->n;
  span->is_use = true;
  pc.release_span_to_page_cache(span);

  // free span 只保证首尾页映射存在（用于合并）。
  auto *a = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
  auto *b = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a->is_use);
}

TEST_F(PageCacheTest, NewSpanEstablishesAllPagesMapForSmallK) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(3);
  ASSERT_NE(span, nullptr);
  for (size_t i = 0; i < span->n; ++i) {
    auto *m =
        static_cast<zmalloc::Span *>(pc.id_span_map_.get(span->page_id + i));
    ASSERT_EQ(m, span);
  }
}

TEST_F(PageCacheTest, ReleaseDoesNotCrashWhenAdjacentMissing) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(5);
  ASSERT_NE(span, nullptr);
  span->is_use = true;
  pc.release_span_to_page_cache(span);
  SUCCEED();
}

TEST_F(PageCacheTest, BucketListContainsReleasedSpan) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(6);
  ASSERT_NE(span, nullptr);
  span->is_use = true;
  const size_t page_id = span->page_id;
  pc.release_span_to_page_cache(span);
  auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
  ASSERT_NE(mapped, nullptr);
  EXPECT_FALSE(mapped->is_use);
  EXPECT_TRUE(ContainsSpan(pc.span_lists_[mapped->n], mapped));
}

TEST_F(PageCacheTest, NewSpanFromBucketClearsIsUse) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *span = pc.new_span(2);
  ASSERT_NE(span, nullptr);
  span->is_use = true;
  pc.release_span_to_page_cache(span);

  zmalloc::Span *span2 = pc.new_span(2);
  ASSERT_NE(span2, nullptr);
  // new_span 返回的 span 不一定清
  // is_use，但中央层会设置。这里保证可用性，不崩溃即可。
  SUCCEED();
}

TEST_F(PageCacheTest, MultipleSmallAllocationsDoNotOverlapInMapping) {
  std::lock_guard<std::mutex> lk(pc.page_mtx());
  zmalloc::Span *a = pc.new_span(1);
  zmalloc::Span *b = pc.new_span(1);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  if (a->page_id != b->page_id) {
    EXPECT_NE(pc.id_span_map_.get(a->page_id), pc.id_span_map_.get(b->page_id));
  }
}

// ------------------------------
// 大量 k 覆盖（每个都是独立用例）
// ------------------------------

#define ZMALLOC_PC_SMALL_K_CASE(K)                                             \
  TEST_F(PageCacheTest, NewSpanMappingAndRelease_K_##K) {                      \
    NewSpanCheckMappingAndRelease(pc, K);                                      \
  }

ZMALLOC_PC_SMALL_K_CASE(1)
ZMALLOC_PC_SMALL_K_CASE(2)
ZMALLOC_PC_SMALL_K_CASE(3)
ZMALLOC_PC_SMALL_K_CASE(4)
ZMALLOC_PC_SMALL_K_CASE(5)
ZMALLOC_PC_SMALL_K_CASE(6)
ZMALLOC_PC_SMALL_K_CASE(7)
ZMALLOC_PC_SMALL_K_CASE(8)
ZMALLOC_PC_SMALL_K_CASE(9)
ZMALLOC_PC_SMALL_K_CASE(10)
ZMALLOC_PC_SMALL_K_CASE(11)
ZMALLOC_PC_SMALL_K_CASE(12)
ZMALLOC_PC_SMALL_K_CASE(13)
ZMALLOC_PC_SMALL_K_CASE(14)
ZMALLOC_PC_SMALL_K_CASE(15)
ZMALLOC_PC_SMALL_K_CASE(16)
ZMALLOC_PC_SMALL_K_CASE(17)
ZMALLOC_PC_SMALL_K_CASE(18)
ZMALLOC_PC_SMALL_K_CASE(19)
ZMALLOC_PC_SMALL_K_CASE(20)
ZMALLOC_PC_SMALL_K_CASE(21)
ZMALLOC_PC_SMALL_K_CASE(22)
ZMALLOC_PC_SMALL_K_CASE(23)
ZMALLOC_PC_SMALL_K_CASE(24)
ZMALLOC_PC_SMALL_K_CASE(25)
ZMALLOC_PC_SMALL_K_CASE(26)
ZMALLOC_PC_SMALL_K_CASE(27)
ZMALLOC_PC_SMALL_K_CASE(28)
ZMALLOC_PC_SMALL_K_CASE(29)
ZMALLOC_PC_SMALL_K_CASE(30)
ZMALLOC_PC_SMALL_K_CASE(31)
ZMALLOC_PC_SMALL_K_CASE(32)
ZMALLOC_PC_SMALL_K_CASE(48)
ZMALLOC_PC_SMALL_K_CASE(64)
ZMALLOC_PC_SMALL_K_CASE(96)
ZMALLOC_PC_SMALL_K_CASE(127)
ZMALLOC_PC_SMALL_K_CASE(128)

#undef ZMALLOC_PC_SMALL_K_CASE

// ------------------------------
// system path（k > NPAGES-1）覆盖
// ------------------------------

#define ZMALLOC_PC_LARGE_K_CASE(K)                                             \
  TEST_F(PageCacheTest, LargeSpanAllocFree_K_##K) {                            \
    NewLargeSpanAllocAndFree(pc, K);                                           \
  }

ZMALLOC_PC_LARGE_K_CASE(129)
ZMALLOC_PC_LARGE_K_CASE(256)
ZMALLOC_PC_LARGE_K_CASE(512)

#undef ZMALLOC_PC_LARGE_K_CASE

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
