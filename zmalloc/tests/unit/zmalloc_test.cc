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

// 大块内存分配释放测试 (257KB)
TEST_F(ZmallocTest, LargeBlockAlloc257KB) {
  constexpr size_t size = 257 * 1024;
  void *ptr = zmalloc(size);
  EXPECT_NE(ptr, nullptr);
  // 验证可写
  std::memset(ptr, 0xAB, size);
  zfree(ptr);
}

// 大块内存分配释放测试 (1MB)
TEST_F(ZmallocTest, LargeBlockAlloc1MB) {
  constexpr size_t size = 1024 * 1024;
  void *ptr = zmalloc(size);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0xCD, size);
  zfree(ptr);
}

// 多次大块内存分配释放测试
TEST_F(ZmallocTest, LargeBlockMultipleAllocFree) {
  constexpr size_t size = 512 * 1024; // 512KB
  for (int i = 0; i < 10; ++i) {
    void *ptr = zmalloc(size);
    EXPECT_NE(ptr, nullptr);
    std::memset(ptr, 0xEF, size);
    zfree(ptr);
  }
}

// 交替分配多个大块内存后释放
TEST_F(ZmallocTest, LargeBlockAlternateAllocThenFree) {
  constexpr size_t size1 = 300 * 1024;
  constexpr size_t size2 = 400 * 1024;
  constexpr size_t size3 = 500 * 1024;

  void *p1 = zmalloc(size1);
  void *p2 = zmalloc(size2);
  void *p3 = zmalloc(size3);

  EXPECT_NE(p1, nullptr);
  EXPECT_NE(p2, nullptr);
  EXPECT_NE(p3, nullptr);

  std::memset(p1, 0x11, size1);
  std::memset(p2, 0x22, size2);
  std::memset(p3, 0x33, size3);

  zfree(p2); // 先释放中间的
  zfree(p1);
  zfree(p3);
}

// 超大块内存测试 (2MB)
TEST_F(ZmallocTest, VeryLargeBlockAlloc2MB) {
  constexpr size_t size = 2 * 1024 * 1024;
  void *ptr = zmalloc(size);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0xDD, size);
  zfree(ptr);
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
