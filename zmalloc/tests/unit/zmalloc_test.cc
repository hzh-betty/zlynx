/**
 * @file zmalloc_test.cc
 * @brief zmalloc 主接口单元测试
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "zmalloc.h"

namespace zmalloc {
namespace {

class ZmallocTest : public ::testing::Test {};

// 功能正确性测试
TEST_F(ZmallocTest, AllocateBasic) {
  void *ptr = Allocate(100);
  ASSERT_NE(ptr, nullptr);
  Deallocate(ptr);
}

TEST_F(ZmallocTest, AllocateVarious) {
  std::vector<size_t> sizes = {1,   8,    16,   32,   64,   128,   256,
                               512, 1024, 2048, 4096, 8192, 16384, 32768};
  std::vector<void *> ptrs;

  for (size_t size : sizes) {
    void *ptr = Allocate(size);
    ASSERT_NE(ptr, nullptr) << "Failed to allocate size: " << size;
    ptrs.push_back(ptr);
  }

  for (void *ptr : ptrs) {
    Deallocate(ptr);
  }
}

TEST_F(ZmallocTest, AllocateLarge) {
  // 超过 32KB
  void *ptr = Allocate(64 * 1024);
  ASSERT_NE(ptr, nullptr);
  Deallocate(ptr);
}

TEST_F(ZmallocTest, AllocateWriteRead) {
  const size_t size = 1024;
  char *ptr = static_cast<char *>(Allocate(size));
  ASSERT_NE(ptr, nullptr);

  // 写入数据
  std::memset(ptr, 0xAB, size);

  // 验证数据
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(ptr[i]), 0xAB);
  }

  Deallocate(ptr);
}

TEST_F(ZmallocTest, DeallocateNull) {
  // 不应该崩溃
  Deallocate(nullptr);
}

TEST_F(ZmallocTest, AllocateZero) {
  void *ptr = Allocate(0);
  // 可能返回 nullptr 或最小分配
  if (ptr != nullptr) {
    Deallocate(ptr);
  }
}

// Reallocate 测试
TEST_F(ZmallocTest, ReallocateNull) {
  // 相当于 Allocate
  void *ptr = Reallocate(nullptr, 100);
  ASSERT_NE(ptr, nullptr);
  Deallocate(ptr);
}

TEST_F(ZmallocTest, ReallocateToZero) {
  void *ptr = Allocate(100);
  ASSERT_NE(ptr, nullptr);

  // 相当于 Deallocate
  void *result = Reallocate(ptr, 0);
  EXPECT_EQ(result, nullptr);
}

TEST_F(ZmallocTest, ReallocateGrow) {
  const size_t old_size = 100;
  const size_t new_size = 200;

  char *ptr = static_cast<char *>(Allocate(old_size));
  ASSERT_NE(ptr, nullptr);

  // 写入数据
  std::memset(ptr, 0xCD, old_size);

  char *new_ptr = static_cast<char *>(Reallocate(ptr, new_size));
  ASSERT_NE(new_ptr, nullptr);

  // 验证原数据保留
  for (size_t i = 0; i < old_size; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(new_ptr[i]), 0xCD);
  }

  Deallocate(new_ptr);
}

TEST_F(ZmallocTest, ReallocateShrink) {
  const size_t old_size = 200;
  const size_t new_size = 100;

  char *ptr = static_cast<char *>(Allocate(old_size));
  ASSERT_NE(ptr, nullptr);

  std::memset(ptr, 0xEF, old_size);

  char *new_ptr = static_cast<char *>(Reallocate(ptr, new_size));
  ASSERT_NE(new_ptr, nullptr);

  // 数据应保留（可能原地缩小）
  for (size_t i = 0; i < new_size; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(new_ptr[i]), 0xEF);
  }

  Deallocate(new_ptr);
}

// AllocateZero 测试
TEST_F(ZmallocTest, AllocateZeroBasic) {
  char *ptr = static_cast<char *>(AllocateZero(10, 10));
  ASSERT_NE(ptr, nullptr);

  // 验证清零
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(ptr[i], 0);
  }

  Deallocate(ptr);
}

TEST_F(ZmallocTest, AllocateZeroZeroSize) {
  void *ptr = AllocateZero(0, 0);
  if (ptr != nullptr) {
    Deallocate(ptr);
  }
}

// AllocateAligned 测试
TEST_F(ZmallocTest, AllocateAligned8) {
  void *ptr = AllocateAligned(100, 8);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0);
  Deallocate(ptr);
}

TEST_F(ZmallocTest, AllocateAligned16) {
  void *ptr = AllocateAligned(100, 16);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 16, 0);

  // 对齐分配需要特殊释放逻辑，这里简化处理
  // 实际使用中需要追踪原始指针
}

TEST_F(ZmallocTest, AllocateAligned64) {
  void *ptr = AllocateAligned(256, 64);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0);
}

// GetAllocatedSize 测试
TEST_F(ZmallocTest, GetAllocatedSizeSmall) {
  void *ptr = Allocate(100);
  ASSERT_NE(ptr, nullptr);

  size_t actual = GetAllocatedSize(ptr);
  EXPECT_GE(actual, 100);

  Deallocate(ptr);
}

TEST_F(ZmallocTest, GetAllocatedSizeLarge) {
  void *ptr = Allocate(64 * 1024);
  ASSERT_NE(ptr, nullptr);

  size_t actual = GetAllocatedSize(ptr);
  EXPECT_GE(actual, 64 * 1024);

  Deallocate(ptr);
}

TEST_F(ZmallocTest, GetAllocatedSizeNull) {
  EXPECT_EQ(GetAllocatedSize(nullptr), 0);
}

// 边界测试
TEST_F(ZmallocTest, AllocateMany) {
  const int count = 10000;
  std::vector<void *> ptrs;
  ptrs.reserve(count);

  for (int i = 0; i < count; ++i) {
    void *ptr = Allocate((i % 1000) + 1);
    ASSERT_NE(ptr, nullptr);
    ptrs.push_back(ptr);
  }

  for (void *ptr : ptrs) {
    Deallocate(ptr);
  }
}

TEST_F(ZmallocTest, AllocateDeallocateAlternating) {
  for (int i = 0; i < 1000; ++i) {
    void *ptr = Allocate(128);
    ASSERT_NE(ptr, nullptr);
    Deallocate(ptr);
  }
}

// 错误处理测试
TEST_F(ZmallocTest, AllocateVeryLarge) {
  // 分配非常大的内存可能失败
  void *ptr = Allocate(1024ULL * 1024 * 1024 * 10); // 10GB
  // 可能成功也可能失败，取决于系统
  if (ptr != nullptr) {
    Deallocate(ptr);
  }
}

} // namespace
} // namespace zmalloc
