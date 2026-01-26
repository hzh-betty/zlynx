#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common.h"
#include "page_cache.h"

// 先 include common/page_cache，避免 private->public 影响标准库头
#define private public
#include "central_cache.h"
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

static size_t CountSpans(zmalloc::SpanList &list) {
  size_t n = 0;
  for (zmalloc::Span *it = list.begin(); it != list.end(); it = it->next) {
    ++n;
  }
  return n;
}

static size_t CountChain(void *start) {
  size_t n = 0;
  while (start) {
    start = zmalloc::next_obj(start);
    ++n;
  }
  return n;
}

static void FetchAndRelease(zmalloc::CentralCache &cc, size_t batch_n,
                            size_t size) {
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, batch_n, size);
  ASSERT_GE(got, 1u);
  ASSERT_LE(got, batch_n);
  ASSERT_NE(start, nullptr);
  ASSERT_NE(end, nullptr);
  ASSERT_EQ(CountChain(start), got);
  ASSERT_EQ(zmalloc::next_obj(end), nullptr);
  cc.release_list_to_spans(start, size);
}

static void FetchTwiceConcatAndRelease(zmalloc::CentralCache &cc, size_t n1,
                                      size_t n2, size_t size) {
  void *s1 = nullptr;
  void *e1 = nullptr;
  const size_t g1 = cc.fetch_range_obj(s1, e1, n1, size);
  ASSERT_GE(g1, 1u);
  ASSERT_EQ(zmalloc::next_obj(e1), nullptr);

  void *s2 = nullptr;
  void *e2 = nullptr;
  const size_t g2 = cc.fetch_range_obj(s2, e2, n2, size);
  ASSERT_GE(g2, 1u);
  ASSERT_EQ(zmalloc::next_obj(e2), nullptr);

  // 拼接链表：s1 -> ... -> e1 -> s2 -> ... -> e2
  zmalloc::next_obj(e1) = s2;
  cc.release_list_to_spans(s1, size);
}

static void ReverseChain(void *&start, void *&end) {
  void *prev = nullptr;
  void *cur = start;
  end = start;
  while (cur) {
    void *next = zmalloc::next_obj(cur);
    zmalloc::next_obj(cur) = prev;
    prev = cur;
    cur = next;
  }
  start = prev;
}

} // namespace

class CentralCacheTest : public ::testing::Test {
protected:
  zmalloc::CentralCache &cc = zmalloc::CentralCache::get_instance();
  zmalloc::PageCache &pc = zmalloc::PageCache::get_instance();
};

TEST_F(CentralCacheTest, FetchReturnsAtLeastOne) {
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, 1, 64);
  ASSERT_GE(got, 1u);
  ASSERT_NE(start, nullptr);
  ASSERT_NE(end, nullptr);
  zmalloc::next_obj(end) = nullptr;
  cc.release_list_to_spans(start, 64);
}

TEST_F(CentralCacheTest, FetchRespectsUpperBoundN) {
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, 8, 64);
  ASSERT_GE(got, 1u);
  ASSERT_LE(got, 8u);
  zmalloc::next_obj(end) = nullptr;
  cc.release_list_to_spans(start, 64);
}

TEST_F(CentralCacheTest, FetchProducesValidChain) {
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, 16, 64);
  ASSERT_EQ(CountChain(start), got);
  ASSERT_EQ(zmalloc::next_obj(end), nullptr);
  cc.release_list_to_spans(start, 64);
}

TEST_F(CentralCacheTest, SpanObjSizeMatchesRequest) {
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, 4, 128);
  ASSERT_GE(got, 1u);
  zmalloc::Span *span = pc.map_object_to_span(start);
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->obj_size, zmalloc::SizeClass::round_up_fast(128));
  cc.release_list_to_spans(start, 128);
}

TEST_F(CentralCacheTest, EmptyNonEmptyListsInitiallySmall) {
  const size_t index = zmalloc::SizeClass::index_fast(64);
  // 刚开始不要求为空，但至少结构可访问。
  EXPECT_LE(CountSpans(cc.free_lists_[index].nonempty) + CountSpans(cc.free_lists_[index].empty), 1024u);
}

TEST_F(CentralCacheTest, DrainSpanMovesToEmptyList) {
  const size_t size = 64;
  const size_t index = zmalloc::SizeClass::index_fast(size);

  void *start = nullptr;
  void *end = nullptr;
  cc.fetch_range_obj(start, end, 1, size);
  zmalloc::Span *span = pc.map_object_to_span(start);
  ASSERT_NE(span, nullptr);

  // 估算该 span 的容量并一次性取尽。
  const size_t capacity = (span->n << zmalloc::PAGE_SHIFT) / span->obj_size;
  ASSERT_GE(capacity, 1u);
  const size_t remaining = capacity - 1;

  void *all_start = nullptr;
  void *all_end = nullptr;
  const size_t got = cc.fetch_range_obj(all_start, all_end, remaining, size);
  ASSERT_EQ(got, remaining);
  ASSERT_EQ(span->free_list, nullptr);

  // span 应当被放入 empty
  EXPECT_TRUE(ContainsSpan(cc.free_lists_[index].empty, span));
  EXPECT_FALSE(ContainsSpan(cc.free_lists_[index].nonempty, span));

  // 归还一个对象（第一次 fetch 的对象）：empty -> nonempty
  cc.release_list_to_spans(start, size);

  EXPECT_TRUE(ContainsSpan(cc.free_lists_[index].nonempty, span));

  // 归还剩余对象，清理状态
  cc.release_list_to_spans(all_start, size);
}

TEST_F(CentralCacheTest, ReleaseBatchingDoesNotLoseObjects) {
  const size_t size = 64;
  void *start = nullptr;
  void *end = nullptr;
  const size_t got = cc.fetch_range_obj(start, end, 64, size);
  ASSERT_GE(got, 1u);
  ASSERT_EQ(CountChain(start), got);
  cc.release_list_to_spans(start, size);
  SUCCEED();
}

TEST_F(CentralCacheTest, MultipleSizeClassesIndependent) {
  void *s1 = nullptr;
  void *e1 = nullptr;
  void *s2 = nullptr;
  void *e2 = nullptr;
  cc.fetch_range_obj(s1, e1, 8, 64);
  cc.fetch_range_obj(s2, e2, 8, 128);
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);
  zmalloc::Span *a = pc.map_object_to_span(s1);
  zmalloc::Span *b = pc.map_object_to_span(s2);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a->obj_size, b->obj_size);
  cc.release_list_to_spans(s1, 64);
  cc.release_list_to_spans(s2, 128);
}

TEST_F(CentralCacheTest, FetchAfterReleaseStillWorks) {
  void *s = nullptr;
  void *e = nullptr;
  cc.fetch_range_obj(s, e, 16, 64);
  cc.release_list_to_spans(s, 64);

  void *s2 = nullptr;
  void *e2 = nullptr;
  const size_t got2 = cc.fetch_range_obj(s2, e2, 16, 64);
  ASSERT_GE(got2, 1u);
  cc.release_list_to_spans(s2, 64);
}

TEST_F(CentralCacheTest, ReleaseNullIsNoop) {
  cc.release_list_to_spans(nullptr, 64);
  SUCCEED();
}

TEST_F(CentralCacheTest, FetchDifferentThreadsDoesNotCrash) {
  // 单测里不做重性能压力，只验证跨线程调用可用。
  std::vector<void *> ptrs;

  std::thread t([&] {
    void *s = nullptr;
    void *e = nullptr;
    const size_t got = cc.fetch_range_obj(s, e, 32, 64);
    ptrs.reserve(got);
    void *cur = s;
    while (cur) {
      ptrs.push_back(cur);
      cur = zmalloc::next_obj(cur);
    }
  });
  t.join();

  // 归还（单链表还原）
  for (size_t i = 0; i + 1 < ptrs.size(); ++i) {
    zmalloc::next_obj(ptrs[i]) = ptrs[i + 1];
  }
  if (!ptrs.empty()) {
    zmalloc::next_obj(ptrs.back()) = nullptr;
    cc.release_list_to_spans(ptrs.front(), 64);
  }
  SUCCEED();
}

TEST_F(CentralCacheTest, NonEmptyListHeadHasFreeObjectsWhenPresent) {
  const size_t size = 64;
  const size_t index = zmalloc::SizeClass::index_fast(size);

  void *s = nullptr;
  void *e = nullptr;
  cc.fetch_range_obj(s, e, 1, size);

  // 若 nonempty 非空，则其 head 必须有 free_list。
  zmalloc::Span *front = cc.free_lists_[index].nonempty.begin();
  if (front != cc.free_lists_[index].nonempty.end()) {
    EXPECT_NE(front->free_list, nullptr);
  }

  cc.release_list_to_spans(s, size);
}

TEST_F(CentralCacheTest, EmptyListSpansHaveNoFreeObjectsWhenPresent) {
  const size_t size = 64;
  const size_t index = zmalloc::SizeClass::index_fast(size);

  // 尝试制造一个 empty span（与 DrainSpanMovesToEmptyList 类似，但更弱断言）
  void *s = nullptr;
  void *e = nullptr;
  cc.fetch_range_obj(s, e, 1, size);
  zmalloc::Span *span = pc.map_object_to_span(s);
  const size_t capacity = (span->n << zmalloc::PAGE_SHIFT) / span->obj_size;
  void *all_s = nullptr;
  void *all_e = nullptr;
  cc.fetch_range_obj(all_s, all_e, capacity, size);

  // empty 链表里出现的 span，其 free_list 应为空。
  for (zmalloc::Span *it = cc.free_lists_[index].empty.begin(); it != cc.free_lists_[index].empty.end(); it = it->next) {
    EXPECT_EQ(it->free_list, nullptr);
  }

  cc.release_list_to_spans(all_s, size);
}

// ------------------------------
// 大量 size/batch 覆盖（每个都是独立用例）
// ------------------------------

#define ZMALLOC_CC_FETCH_RELEASE(SZ, N)                                        \
  TEST_F(CentralCacheTest, FetchRelease_Size_##SZ##_Batch_##N) {               \
    FetchAndRelease(cc, N, SZ);                                                \
  }

// 小对象/边界
ZMALLOC_CC_FETCH_RELEASE(8, 1)
ZMALLOC_CC_FETCH_RELEASE(8, 8)
ZMALLOC_CC_FETCH_RELEASE(8, 64)
ZMALLOC_CC_FETCH_RELEASE(16, 1)
ZMALLOC_CC_FETCH_RELEASE(16, 32)
ZMALLOC_CC_FETCH_RELEASE(32, 1)
ZMALLOC_CC_FETCH_RELEASE(32, 32)
ZMALLOC_CC_FETCH_RELEASE(64, 1)
ZMALLOC_CC_FETCH_RELEASE(64, 2)
ZMALLOC_CC_FETCH_RELEASE(64, 8)
ZMALLOC_CC_FETCH_RELEASE(64, 32)
ZMALLOC_CC_FETCH_RELEASE(64, 64)
ZMALLOC_CC_FETCH_RELEASE(96, 8)
ZMALLOC_CC_FETCH_RELEASE(128, 1)
ZMALLOC_CC_FETCH_RELEASE(128, 16)
ZMALLOC_CC_FETCH_RELEASE(256, 1)
ZMALLOC_CC_FETCH_RELEASE(256, 8)
ZMALLOC_CC_FETCH_RELEASE(512, 1)
ZMALLOC_CC_FETCH_RELEASE(512, 8)
ZMALLOC_CC_FETCH_RELEASE(1024, 1)
ZMALLOC_CC_FETCH_RELEASE(1024, 4)
ZMALLOC_CC_FETCH_RELEASE(2048, 1)
ZMALLOC_CC_FETCH_RELEASE(2048, 2)
ZMALLOC_CC_FETCH_RELEASE(4096, 1)
ZMALLOC_CC_FETCH_RELEASE(4096, 2)
ZMALLOC_CC_FETCH_RELEASE(8192, 1)
ZMALLOC_CC_FETCH_RELEASE(8192, 2)
ZMALLOC_CC_FETCH_RELEASE(65536, 1)
ZMALLOC_CC_FETCH_RELEASE(65536, 2)
ZMALLOC_CC_FETCH_RELEASE(131072, 1)
ZMALLOC_CC_FETCH_RELEASE(131072, 2)
ZMALLOC_CC_FETCH_RELEASE(262144, 1)

#undef ZMALLOC_CC_FETCH_RELEASE

// ------------------------------
// 组合链表 / 乱序链表 回归
// ------------------------------

TEST_F(CentralCacheTest, FetchTwiceConcatThenRelease) {
  FetchTwiceConcatAndRelease(cc, 8, 8, 64);
}

TEST_F(CentralCacheTest, FetchTwiceConcatDifferentBatchThenRelease) {
  FetchTwiceConcatAndRelease(cc, 1, 32, 128);
}

TEST_F(CentralCacheTest, ReleaseReversedChainDoesNotCrash) {
  void *s = nullptr;
  void *e = nullptr;
  const size_t got = cc.fetch_range_obj(s, e, 32, 64);
  ASSERT_GE(got, 1u);
  ASSERT_EQ(zmalloc::next_obj(e), nullptr);
  void *rev_end = nullptr;
  ReverseChain(s, rev_end);
  // 反转后仍应是合法单链表
  ASSERT_EQ(CountChain(s), got);
  cc.release_list_to_spans(s, 64);
}

TEST_F(CentralCacheTest, ReleaseInterleavedFetchChainsDoesNotCrash) {
  void *s1 = nullptr;
  void *e1 = nullptr;
  const size_t g1 = cc.fetch_range_obj(s1, e1, 8, 64);
  ASSERT_GE(g1, 1u);

  void *s2 = nullptr;
  void *e2 = nullptr;
  const size_t g2 = cc.fetch_range_obj(s2, e2, 8, 64);
  ASSERT_GE(g2, 1u);

  // 交错拼接：从 s1 和 s2 交替取节点构成新链表
  void *head = nullptr;
  void *tail = nullptr;
  void *a = s1;
  void *b = s2;
  while (a || b) {
    void *take = nullptr;
    if (a) {
      take = a;
      a = zmalloc::next_obj(a);
    } else {
      take = b;
      b = zmalloc::next_obj(b);
    }
    if (!head) {
      head = tail = take;
      zmalloc::next_obj(tail) = nullptr;
    } else {
      zmalloc::next_obj(tail) = take;
      tail = take;
      zmalloc::next_obj(tail) = nullptr;
    }
  }

  ASSERT_EQ(CountChain(head), g1 + g2);
  cc.release_list_to_spans(head, 64);
}

TEST_F(CentralCacheTest, DifferentSizeClassesInterleavedDoesNotCrash) {
  void *a1 = nullptr;
  void *a2 = nullptr;
  cc.fetch_range_obj(a1, a2, 16, 64);
  void *b1 = nullptr;
  void *b2 = nullptr;
  cc.fetch_range_obj(b1, b2, 16, 128);
  cc.release_list_to_spans(a1, 64);
  cc.release_list_to_spans(b1, 128);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
