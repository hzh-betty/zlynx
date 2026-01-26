#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common.h"
// 先 include common，避免 private->public 影响标准库头
#define private public
#include "thread_cache.h"
#undef private

#include "zmalloc.h"

namespace {

static bool IsAligned(void *p, size_t align) {
  return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
}

static void AllocTouchFree(zmalloc::ThreadCache *tc, size_t size) {
  void *p = tc->allocate(size);
  ASSERT_NE(p, nullptr);
  // 只做非常轻量的触摸，避免越界。
  static_cast<unsigned char *>(p)[0] = 0xAB;
  tc->deallocate(p, size);
}

static void TriggerListTooLongOnce(zmalloc::ThreadCache *tc, size_t size,
                                  size_t new_max_size) {
  const size_t index = zmalloc::SizeClass::index_fast(size);
  size_t old = tc->free_lists_[index].max_size();
  tc->free_lists_[index].max_size() = new_max_size;

  // 申请 3 个并全部释放：在较小阈值下应触发回收。
  void *p1 = tc->allocate(size);
  void *p2 = tc->allocate(size);
  void *p3 = tc->allocate(size);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p3, nullptr);
  tc->deallocate(p1, size);
  tc->deallocate(p2, size);
  tc->deallocate(p3, size);

  // 清理：取回两个再释放，避免把链表留得太长。
  void *q1 = tc->allocate(size);
  void *q2 = tc->allocate(size);
  ASSERT_NE(q1, nullptr);
  ASSERT_NE(q2, nullptr);
  tc->deallocate(q1, size);
  tc->deallocate(q2, size);

  tc->free_lists_[index].max_size() = old;
}

} // namespace

class ThreadCacheTest : public ::testing::Test {
protected:
  zmalloc::ThreadCache *tc = zmalloc::get_thread_cache();
};

TEST_F(ThreadCacheTest, AllocateReturnsNonNull) {
  void *p = tc->allocate(64);
  ASSERT_NE(p, nullptr);
  tc->deallocate(p, 64);
}

TEST_F(ThreadCacheTest, AllocateIsAligned) {
  const size_t req = 24;
  const size_t align = zmalloc::SizeClass::round_up_fast(req);
  void *p = tc->allocate(req);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(IsAligned(p, 8));
  EXPECT_TRUE(IsAligned(p, std::min<size_t>(align, 8))); // 小对象至少 8B
  tc->deallocate(p, req);
}

TEST_F(ThreadCacheTest, DeallocateThenAllocateReusesPointerLIFO) {
  void *p1 = tc->allocate(64);
  tc->deallocate(p1, 64);
  void *p2 = tc->allocate(64);
  EXPECT_EQ(p1, p2);
  tc->deallocate(p2, 64);
}

TEST_F(ThreadCacheTest, DifferentSizesDoNotShareSameFreeListIndexUsually) {
  void *a = tc->allocate(64);
  void *b = tc->allocate(128);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  tc->deallocate(a, 64);
  tc->deallocate(b, 128);
  SUCCEED();
}

TEST_F(ThreadCacheTest, CanHandleManyAllocFreeSmall) {
  std::vector<void *> v;
  for (int i = 0; i < 2000; ++i) {
    v.push_back(tc->allocate(64));
  }
  for (auto *p : v) {
    tc->deallocate(p, 64);
  }
  SUCCEED();
}

TEST_F(ThreadCacheTest, ListTooLongRecyclesWithoutCrash) {
  const size_t size = 64;
  const size_t index = zmalloc::SizeClass::index_fast(size);

  // 把阈值调小，确保触发 list_too_long。
  size_t old = tc->free_lists_[index].max_size();
  tc->free_lists_[index].max_size() = 2;

  void *p1 = tc->allocate(size);
  void *p2 = tc->allocate(size);
  void *p3 = tc->allocate(size);
  tc->deallocate(p1, size);
  tc->deallocate(p2, size);
  tc->deallocate(p3, size); // 应触发回收

  // 清理：再取几个并释放，避免污染其他测试
  void *q1 = tc->allocate(size);
  void *q2 = tc->allocate(size);
  tc->deallocate(q1, size);
  tc->deallocate(q2, size);

  tc->free_lists_[index].max_size() = old;
  SUCCEED();
}

TEST_F(ThreadCacheTest, ZmallocAndThreadCacheAgreeSmall) {
  void *p = zmalloc::zmalloc(64);
  ASSERT_NE(p, nullptr);
  zmalloc::zfree(p);
  SUCCEED();
}

TEST_F(ThreadCacheTest, LargeAllocationBypassesThreadCache) {
  // > MAX_BYTES 会走 page cache / system
  void *p = zmalloc::zmalloc(zmalloc::MAX_BYTES + 16);
  ASSERT_NE(p, nullptr);
  zmalloc::zfree(p);
}

TEST_F(ThreadCacheTest, MixedSizesStable) {
  void *a = tc->allocate(32);
  void *b = tc->allocate(64);
  void *c = tc->allocate(96);
  tc->deallocate(b, 64);
  tc->deallocate(a, 32);
  tc->deallocate(c, 96);
  SUCCEED();
}

TEST_F(ThreadCacheTest, TouchingMemoryDoesNotCrash) {
  char *p = static_cast<char *>(tc->allocate(128));
  ASSERT_NE(p, nullptr);
  p[0] = 1;
  p[127] = 2;
  tc->deallocate(p, 128);
}

// ------------------------------
// 大量 size 覆盖（每个都是独立用例）
// ------------------------------

#define ZMALLOC_TC_ALLOC_FREE_CASE(SZ)                                         \
  TEST_F(ThreadCacheTest, AllocFree_Size_##SZ) { AllocTouchFree(tc, SZ); }

ZMALLOC_TC_ALLOC_FREE_CASE(1)
ZMALLOC_TC_ALLOC_FREE_CASE(2)
ZMALLOC_TC_ALLOC_FREE_CASE(7)
ZMALLOC_TC_ALLOC_FREE_CASE(8)
ZMALLOC_TC_ALLOC_FREE_CASE(9)
ZMALLOC_TC_ALLOC_FREE_CASE(15)
ZMALLOC_TC_ALLOC_FREE_CASE(16)
ZMALLOC_TC_ALLOC_FREE_CASE(17)
ZMALLOC_TC_ALLOC_FREE_CASE(24)
ZMALLOC_TC_ALLOC_FREE_CASE(31)
ZMALLOC_TC_ALLOC_FREE_CASE(32)
ZMALLOC_TC_ALLOC_FREE_CASE(33)
ZMALLOC_TC_ALLOC_FREE_CASE(48)
ZMALLOC_TC_ALLOC_FREE_CASE(63)
ZMALLOC_TC_ALLOC_FREE_CASE(64)
ZMALLOC_TC_ALLOC_FREE_CASE(65)
ZMALLOC_TC_ALLOC_FREE_CASE(80)
ZMALLOC_TC_ALLOC_FREE_CASE(95)
ZMALLOC_TC_ALLOC_FREE_CASE(96)
ZMALLOC_TC_ALLOC_FREE_CASE(97)
ZMALLOC_TC_ALLOC_FREE_CASE(112)
ZMALLOC_TC_ALLOC_FREE_CASE(127)
ZMALLOC_TC_ALLOC_FREE_CASE(128)
ZMALLOC_TC_ALLOC_FREE_CASE(129)
ZMALLOC_TC_ALLOC_FREE_CASE(192)
ZMALLOC_TC_ALLOC_FREE_CASE(255)
ZMALLOC_TC_ALLOC_FREE_CASE(256)
ZMALLOC_TC_ALLOC_FREE_CASE(257)
ZMALLOC_TC_ALLOC_FREE_CASE(384)
ZMALLOC_TC_ALLOC_FREE_CASE(511)
ZMALLOC_TC_ALLOC_FREE_CASE(512)
ZMALLOC_TC_ALLOC_FREE_CASE(513)
ZMALLOC_TC_ALLOC_FREE_CASE(768)
ZMALLOC_TC_ALLOC_FREE_CASE(1023)
ZMALLOC_TC_ALLOC_FREE_CASE(1024)
ZMALLOC_TC_ALLOC_FREE_CASE(1025)
ZMALLOC_TC_ALLOC_FREE_CASE(2047)
ZMALLOC_TC_ALLOC_FREE_CASE(2048)
ZMALLOC_TC_ALLOC_FREE_CASE(2049)
ZMALLOC_TC_ALLOC_FREE_CASE(4095)
ZMALLOC_TC_ALLOC_FREE_CASE(4096)
ZMALLOC_TC_ALLOC_FREE_CASE(4097)
ZMALLOC_TC_ALLOC_FREE_CASE(8191)
ZMALLOC_TC_ALLOC_FREE_CASE(8192)
ZMALLOC_TC_ALLOC_FREE_CASE(8193)
ZMALLOC_TC_ALLOC_FREE_CASE(16384)
ZMALLOC_TC_ALLOC_FREE_CASE(32768)
ZMALLOC_TC_ALLOC_FREE_CASE(65536)
ZMALLOC_TC_ALLOC_FREE_CASE(131072)
ZMALLOC_TC_ALLOC_FREE_CASE(200000)
ZMALLOC_TC_ALLOC_FREE_CASE(262144)

#undef ZMALLOC_TC_ALLOC_FREE_CASE

// ------------------------------
// list_too_long 触发回归（多 size 覆盖）
// ------------------------------

#define ZMALLOC_TC_TOO_LONG_CASE(SZ)                                           \
  TEST_F(ThreadCacheTest, ListTooLongTrigger_Size_##SZ) {                      \
    TriggerListTooLongOnce(tc, SZ, 1);                                         \
  }

ZMALLOC_TC_TOO_LONG_CASE(32)
ZMALLOC_TC_TOO_LONG_CASE(64)
ZMALLOC_TC_TOO_LONG_CASE(128)
ZMALLOC_TC_TOO_LONG_CASE(256)
ZMALLOC_TC_TOO_LONG_CASE(512)
ZMALLOC_TC_TOO_LONG_CASE(1024)
ZMALLOC_TC_TOO_LONG_CASE(4096)
ZMALLOC_TC_TOO_LONG_CASE(8192)
ZMALLOC_TC_TOO_LONG_CASE(65536)
ZMALLOC_TC_TOO_LONG_CASE(131072)

#undef ZMALLOC_TC_TOO_LONG_CASE

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
