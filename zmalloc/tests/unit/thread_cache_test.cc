/**
 * @file thread_cache_test.cc
 * @brief ThreadCache 单元测试
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "common.h"
#include "size_class.h"
#include "thread_cache.h"

namespace zmalloc {
namespace {

class ThreadCacheTest : public ::testing::Test {
protected:
  ThreadCache *tc_ = ThreadCache::GetInstance();
};

// 功能正确性测试
TEST_F(ThreadCacheTest, AllocateSmall) {
  void *ptr = tc_->Allocate(16);
  ASSERT_NE(ptr, nullptr);

  // 验证对齐
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0);

  tc_->Deallocate(ptr, 16);
}

TEST_F(ThreadCacheTest, AllocateVarious) {
  std::vector<void *> ptrs;
  std::vector<size_t> sizes = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};

  for (size_t size : sizes) {
    void *ptr = tc_->Allocate(size);
    ASSERT_NE(ptr, nullptr) << "Failed to allocate size: " << size;
    ptrs.push_back(ptr);
  }

  for (size_t i = 0; i < ptrs.size(); ++i) {
    tc_->Deallocate(ptrs[i], sizes[i]);
  }
}

TEST_F(ThreadCacheTest, AllocateManySmall) {
  const int count = 1000;
  std::vector<void *> ptrs;
  ptrs.reserve(count);

  for (int i = 0; i < count; ++i) {
    void *ptr = tc_->Allocate(32);
    ASSERT_NE(ptr, nullptr);
    ptrs.push_back(ptr);
  }

  for (void *ptr : ptrs) {
    tc_->Deallocate(ptr, 32);
  }
}

TEST_F(ThreadCacheTest, AllocateLarge) {
  // 大于 kMaxCacheableSize，直接走 PageHeap
  void *ptr = tc_->Allocate(kMaxCacheableSize + 1);
  ASSERT_NE(ptr, nullptr);

  // 大对象需要通过主接口释放
  size_t page_id = reinterpret_cast<size_t>(ptr) >> kPageShift;
  tc_->Deallocate(ptr, kMaxCacheableSize + 1);
}

TEST_F(ThreadCacheTest, ReuseFreedMemory) {
  void *ptr1 = tc_->Allocate(64);
  ASSERT_NE(ptr1, nullptr);

  tc_->Deallocate(ptr1, 64);

  // 再次分配相同大小，应该复用
  void *ptr2 = tc_->Allocate(64);
  ASSERT_NE(ptr2, nullptr);

  tc_->Deallocate(ptr2, 64);
}

TEST_F(ThreadCacheTest, AllocateZero) {
  void *ptr = tc_->Allocate(0);
  ASSERT_NE(ptr, nullptr); // 分配最小大小
  tc_->Deallocate(ptr, 1);
}

TEST_F(ThreadCacheTest, DeallocateNull) {
  // 不应崩溃
  tc_->Deallocate(nullptr, 0);
  tc_->Deallocate(nullptr, 100);
}

// 边界测试
TEST_F(ThreadCacheTest, AllocateMaxCacheable) {
  void *ptr = tc_->Allocate(kMaxCacheableSize);
  ASSERT_NE(ptr, nullptr);
  tc_->Deallocate(ptr, kMaxCacheableSize);
}

TEST_F(ThreadCacheTest, AllocateAlignmentBoundaries) {
  // 测试各对齐边界
  std::vector<size_t> boundaries = {8, 16, 128, 144, 1024, 1152, 8192, 9216};
  for (size_t size : boundaries) {
    void *ptr = tc_->Allocate(size);
    ASSERT_NE(ptr, nullptr) << "Failed at size: " << size;
    tc_->Deallocate(ptr, size);
  }
}

// 线程测试
TEST_F(ThreadCacheTest, EachThreadHasOwnCache) {
  std::atomic<ThreadCache *> other_tc{nullptr};

  std::thread t([&other_tc]() { other_tc.store(ThreadCache::GetInstance()); });

  t.join();

  // 不同线程应该有不同的 ThreadCache
  EXPECT_NE(tc_, other_tc.load());
}

// FreeList 测试
class FreeListTest : public ::testing::Test {
protected:
  FreeList list_;
};

TEST_F(FreeListTest, InitiallyEmpty) {
  EXPECT_TRUE(list_.IsEmpty());
  EXPECT_EQ(list_.Size(), 0);
}

TEST_F(FreeListTest, PushPop) {
  // 使用足够大的对象存储 next 指针 (sizeof(void*))
  void *objects[3];

  list_.Push(&objects[0]);
  EXPECT_FALSE(list_.IsEmpty());
  EXPECT_EQ(list_.Size(), 1);

  list_.Push(&objects[1]);
  list_.Push(&objects[2]);
  EXPECT_EQ(list_.Size(), 3);

  EXPECT_EQ(list_.Pop(), &objects[2]);
  EXPECT_EQ(list_.Pop(), &objects[1]);
  EXPECT_EQ(list_.Pop(), &objects[0]);
  EXPECT_TRUE(list_.IsEmpty());
}

TEST_F(FreeListTest, PopEmpty) { EXPECT_EQ(list_.Pop(), nullptr); }

TEST_F(FreeListTest, MaxSize) {
  EXPECT_EQ(list_.MaxSize(), 1);
  list_.SetMaxSize(100);
  EXPECT_EQ(list_.MaxSize(), 100);
}

} // namespace
} // namespace zmalloc
