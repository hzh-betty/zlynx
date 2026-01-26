/**
 * @file zmalloc_test.cc
 * @brief zmalloc 核心接口单元测试
 */

#include "zmalloc.h"
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <vector>

namespace zmalloc {
namespace {

class ZmallocTest : public ::testing::Test {};

static void AllocTouchFree(size_t size) {
  ASSERT_NE(size, 0u);
  unsigned char *p = static_cast<unsigned char *>(zmalloc(size));
  ASSERT_NE(p, nullptr);
  // 触达不同位置，验证可写且不会越界
  p[0] = 0xA5;
  p[size - 1] = 0x5A;
  if (size > 4) {
    p[size / 2] = 0x3C;
    p[size / 4] = 0xC3;
  }
  zfree(p);
}

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

// 对齐验证测试
TEST_F(ZmallocTest, AlignmentSmall8Byte) {
  for (int i = 0; i < 100; ++i) {
    void *ptr = zmalloc(8);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0u);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, AlignmentMedium16Byte) {
  for (int i = 0; i < 50; ++i) {
    void *ptr = zmalloc(256);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0u);
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, AlignmentLarge) {
  void *ptr = zmalloc(64 * 1024);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0u);
  zfree(ptr);
}

// 内存复用测试
TEST_F(ZmallocTest, MemoryReuse) {
  void *ptr1 = zmalloc(64);
  zfree(ptr1);
  void *ptr2 = zmalloc(64);
  // 可能复用刚释放的内存（取决于实现）
  EXPECT_NE(ptr2, nullptr);
  zfree(ptr2);
}

TEST_F(ZmallocTest, MemoryReuseMultiple) {
  std::vector<void *> ptrs;
  for (int i = 0; i < 10; ++i) {
    ptrs.push_back(zmalloc(128));
  }
  for (void *p : ptrs) {
    zfree(p);
  }
  // 再次分配，应该复用之前的内存
  std::vector<void *> ptrs2;
  for (int i = 0; i < 10; ++i) {
    ptrs2.push_back(zmalloc(128));
    EXPECT_NE(ptrs2.back(), nullptr);
  }
  for (void *p : ptrs2) {
    zfree(p);
  }
}

// 边界大小测试
TEST_F(ZmallocTest, BoundarySizeClass1) {
  // 1 字节 - 最小分配
  void *ptr = zmalloc(1);
  EXPECT_NE(ptr, nullptr);
  *static_cast<char *>(ptr) = 'A';
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundarySizeClass128) {
  // 128 字节 - 第一个区间的边界
  void *ptr = zmalloc(128);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0, 128);
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundarySizeClass129) {
  // 129 字节 - 第二个区间的开始
  void *ptr = zmalloc(129);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0, 129);
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundarySizeClass1024) {
  // 1024 字节 - 第二个区间的边界
  void *ptr = zmalloc(1024);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0, 1024);
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundarySizeClass8KB) {
  // 8KB - 第三个区间的边界
  void *ptr = zmalloc(8 * 1024);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0, 8 * 1024);
  zfree(ptr);
}

TEST_F(ZmallocTest, BoundarySizeClass64KB) {
  // 64KB - 第四个区间的边界
  void *ptr = zmalloc(64 * 1024);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0, 64 * 1024);
  zfree(ptr);
}

// 大量分配测试
TEST_F(ZmallocTest, MassAllocation1000) {
  std::vector<void *> ptrs;
  for (int i = 0; i < 1000; ++i) {
    ptrs.push_back(zmalloc(32));
    EXPECT_NE(ptrs.back(), nullptr);
  }
  for (void *p : ptrs) {
    zfree(p);
  }
}

TEST_F(ZmallocTest, MassAllocationVariousSizes) {
  std::vector<std::pair<void *, size_t>> allocations;
  size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
  for (size_t s : sizes) {
    for (int i = 0; i < 50; ++i) {
      void *ptr = zmalloc(s);
      EXPECT_NE(ptr, nullptr);
      allocations.push_back({ptr, s});
    }
  }
  for (auto &p : allocations) {
    zfree(p.first);
  }
}

// 数据完整性测试
TEST_F(ZmallocTest, DataIntegritySmall) {
  for (size_t size = 1; size <= 128; size *= 2) {
    unsigned char *ptr = static_cast<unsigned char *>(zmalloc(size));
    EXPECT_NE(ptr, nullptr);
    for (size_t i = 0; i < size; ++i) {
      ptr[i] = static_cast<unsigned char>(i & 0xFF);
    }
    for (size_t i = 0; i < size; ++i) {
      EXPECT_EQ(ptr[i], static_cast<unsigned char>(i & 0xFF));
    }
    zfree(ptr);
  }
}

TEST_F(ZmallocTest, DataIntegrityMedium) {
  constexpr size_t size = 4096;
  unsigned char *ptr = static_cast<unsigned char *>(zmalloc(size));
  EXPECT_NE(ptr, nullptr);
  for (size_t i = 0; i < size; ++i) {
    ptr[i] = static_cast<unsigned char>(i & 0xFF);
  }
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(ptr[i], static_cast<unsigned char>(i & 0xFF));
  }
  zfree(ptr);
}

TEST_F(ZmallocTest, DataIntegrityLarge) {
  constexpr size_t size = 512 * 1024;
  unsigned char *ptr = static_cast<unsigned char *>(zmalloc(size));
  EXPECT_NE(ptr, nullptr);
  for (size_t i = 0; i < size; ++i) {
    ptr[i] = static_cast<unsigned char>(i & 0xFF);
  }
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(ptr[i], static_cast<unsigned char>(i & 0xFF));
  }
  zfree(ptr);
}

// 逆序释放测试
TEST_F(ZmallocTest, ReverseOrderFree) {
  std::vector<void *> ptrs;
  for (int i = 0; i < 100; ++i) {
    ptrs.push_back(zmalloc(64));
  }
  for (int i = 99; i >= 0; --i) {
    zfree(ptrs[i]);
  }
}

// 随机顺序释放测试
TEST_F(ZmallocTest, RandomOrderFree) {
  std::vector<void *> ptrs;
  for (int i = 0; i < 100; ++i) {
    ptrs.push_back(zmalloc(64));
  }
  // 伪随机顺序
  int order[] = {50, 25, 75, 12, 37, 62, 87, 6,  18, 31, 43, 56, 68, 81, 93,
                 3,  9,  15, 21, 28, 34, 40, 46, 53, 59, 65, 71, 78, 84, 90,
                 96, 1,  4,  7,  10, 13, 16, 19, 22, 26, 29, 32, 35, 38, 41,
                 44, 47, 51, 54, 57, 60, 63, 66, 69, 72, 76, 79, 82, 85, 88,
                 91, 94, 97, 0,  2,  5,  8,  11, 14, 17, 20, 23, 24, 27, 30,
                 33, 36, 39, 42, 45, 48, 49, 52, 55, 58, 61, 64, 67, 70, 73,
                 74, 77, 80, 83, 86, 89, 92, 95, 98, 99};
  for (int i : order) {
    zfree(ptrs[i]);
  }
}

// 混合大小分配释放
TEST_F(ZmallocTest, MixedSizeAllocFree) {
  std::vector<std::pair<void *, size_t>> allocations;
  for (int i = 0; i < 10; ++i) {
    allocations.push_back({zmalloc(8), 8});
    allocations.push_back({zmalloc(64), 64});
    allocations.push_back({zmalloc(512), 512});
    allocations.push_back({zmalloc(4096), 4096});
    allocations.push_back({zmalloc(32768), 32768});
  }
  for (auto &a : allocations) {
    zfree(a.first);
  }
}

// 连续分配相同大小测试
TEST_F(ZmallocTest, ConsecutiveSameSizeAlloc) {
  constexpr int kCount = 500;
  std::vector<void *> ptrs;
  for (int i = 0; i < kCount; ++i) {
    ptrs.push_back(zmalloc(256));
    EXPECT_NE(ptrs.back(), nullptr);
  }
  // 验证没有重复地址
  std::set<void *> unique_ptrs(ptrs.begin(), ptrs.end());
  EXPECT_EQ(unique_ptrs.size(), static_cast<size_t>(kCount));
  for (void *p : ptrs) {
    zfree(p);
  }
}

// Page 边界测试
TEST_F(ZmallocTest, PageBoundaryAlloc) {
  // 分配刚好一页 (8KB)
  void *ptr = zmalloc(PAGE_SIZE);
  EXPECT_NE(ptr, nullptr);
  std::memset(ptr, 0xAA, PAGE_SIZE);
  zfree(ptr);
}

TEST_F(ZmallocTest, MultiPageAlloc) {
  // 分配多页
  for (size_t pages = 1; pages <= 10; ++pages) {
    void *ptr = zmalloc(pages * PAGE_SIZE);
    EXPECT_NE(ptr, nullptr);
    std::memset(ptr, 0xBB, pages * PAGE_SIZE);
    zfree(ptr);
  }
}

// ------------------------------
// 补充：更多 size 覆盖（边界 + 典型点）
// ------------------------------

#define ZMALLOC_API_TOUCH_CASE(NAME, SIZE)                                    \
  TEST_F(ZmallocTest, AllocTouchFree_##NAME) { AllocTouchFree(static_cast<size_t>(SIZE)); }

ZMALLOC_API_TOUCH_CASE(S2, 2)
ZMALLOC_API_TOUCH_CASE(S7, 7)
ZMALLOC_API_TOUCH_CASE(S15, 15)
ZMALLOC_API_TOUCH_CASE(S31, 31)
ZMALLOC_API_TOUCH_CASE(S33, 33)
ZMALLOC_API_TOUCH_CASE(S127, 127)
ZMALLOC_API_TOUCH_CASE(S255, 255)
ZMALLOC_API_TOUCH_CASE(S1023, 1023)
ZMALLOC_API_TOUCH_CASE(S1025, 1025)
ZMALLOC_API_TOUCH_CASE(S8191, 8191)
ZMALLOC_API_TOUCH_CASE(S8193, 8193)
ZMALLOC_API_TOUCH_CASE(S65535, 65535)
ZMALLOC_API_TOUCH_CASE(S65537, 65537)
ZMALLOC_API_TOUCH_CASE(S262143, 262143)

#undef ZMALLOC_API_TOUCH_CASE

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
