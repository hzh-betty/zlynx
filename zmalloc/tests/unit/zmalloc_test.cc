/**
 * @file zmalloc_test.cc
 * @brief zmalloc 核心接口单元测试
 */

#include "zmalloc.h"
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace zmalloc {
namespace {

class ZmallocTest : public ::testing::Test {};

// 基本分配释放测试
TEST_F(ZmallocTest, BasicAllocFree) {
  void *ptr = zmalloc(64);
  EXPECT_NE(ptr, nullptr);
  zfree(ptr);
}

TEST_F(ZmallocTest, ZeroSizeAlloc) {
  void *ptr = zmalloc(0);
  EXPECT_EQ(ptr, nullptr);
}

TEST_F(ZmallocTest, NullptrFree) {
  zfree(nullptr); // 不应崩溃
}

// 不同大小的分配测试
TEST_F(ZmallocTest, SmallAlloc) {
  // [1, 128] 区间
  std::vector<size_t> sizes = {1, 8, 16, 64, 128};
  for (size_t size : sizes) {
    void *ptr = zmalloc(size);
    EXPECT_NE(ptr, nullptr);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, MediumAlloc) {
  // [129, 1024] 区间
  std::vector<size_t> sizes = {129, 256, 512, 1024};
  for (size_t size : sizes) {
    void *ptr = zmalloc(size);
    EXPECT_NE(ptr, nullptr);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, LargeAlloc) {
  // [1025, 8KB] 区间
  std::vector<size_t> sizes = {1025, 4096, 8 * 1024};
  for (size_t size : sizes) {
    void *ptr = zmalloc(size);
    EXPECT_NE(ptr, nullptr);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, VeryLargeAlloc) {
  // [8KB+1, 256KB] 区间
  std::vector<size_t> sizes = {8 * 1024 + 1, 64 * 1024, 256 * 1024};
  for (size_t size : sizes) {
    void *ptr = zmalloc(size);
    EXPECT_NE(ptr, nullptr);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, HugeAlloc) {
  // > 256KB，走 PageCache 直接分配
  void *ptr = zmalloc(512 * 1024);
  EXPECT_NE(ptr, nullptr);
  zfree(ptr);

  ptr = zmalloc(1024 * 1024);
  EXPECT_NE(ptr, nullptr);
  zfree(ptr);
}

// 内存可写测试
TEST_F(ZmallocTest, MemoryWritable) {
  size_t size = 1024;
  char *ptr = static_cast<char *>(zmalloc(size));
  EXPECT_NE(ptr, nullptr);

  std::memset(ptr, 'A', size);
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(ptr[i], 'A');
  }
  zfree(ptr);
}

// 多次分配释放
TEST_F(ZmallocTest, MultipleAllocFree) {
  std::vector<void *> ptrs;
  for (int i = 0; i < 100; ++i) {
    ptrs.push_back(zmalloc(64));
    EXPECT_NE(ptrs.back(), nullptr);
  }
  for (void *ptr : ptrs) {
    zfree(ptr);
  }
}

// 交替分配释放
TEST_F(ZmallocTest, AlternatingAllocFree) {
  for (int i = 0; i < 50; ++i) {
    void *ptr = zmalloc(128);
    EXPECT_NE(ptr, nullptr);
    zfree(ptr);
  }
}

// 边界值测试
TEST_F(ZmallocTest, BoundaryAllocMaxBytes) {
  // 恰好 MAX_BYTES
  void *ptr = zmalloc(256 * 1024);
  EXPECT_NE(ptr, nullptr);
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundaryAllocMaxBytesPlus1) {
  // MAX_BYTES + 1
  void *ptr = zmalloc(256 * 1024 + 1);
  EXPECT_NE(ptr, nullptr);
  zfree(ptr);
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
