/**
 * @file central_cache_test.cc
 * @brief CentralCache 单元测试
 */

#include <gtest/gtest.h>

#include "central_cache.h"
#include "size_class.h"

namespace zmalloc {
namespace {

class CentralCacheTest : public ::testing::Test {
protected:
  CentralCache &cc_ = CentralCache::Instance();
};

// 功能正确性测试
TEST_F(CentralCacheTest, FetchRangeBasic) {
  size_t class_index = 0; // 8 字节类
  size_t actual_count = 0;

  void *start = cc_.FetchRange(class_index, 10, &actual_count);
  ASSERT_NE(start, nullptr);
  EXPECT_GT(actual_count, 0);
  EXPECT_LE(actual_count, 10);

  cc_.ReleaseRange(class_index, start, actual_count);
}

TEST_F(CentralCacheTest, FetchRangeVarious) {
  for (size_t i = 0; i < 10; ++i) {
    size_t actual_count = 0;
    void *start = cc_.FetchRange(i, 5, &actual_count);

    if (start != nullptr && actual_count > 0) {
      cc_.ReleaseRange(i, start, actual_count);
    }
  }
}

TEST_F(CentralCacheTest, FetchAndRelease) {
  size_t class_index = 2; // 24 字节类
  size_t actual_count = 0;

  void *start = cc_.FetchRange(class_index, 20, &actual_count);
  ASSERT_NE(start, nullptr);
  EXPECT_GT(actual_count, 0);

  // 释放全部
  cc_.ReleaseRange(class_index, start, actual_count);
}

TEST_F(CentralCacheTest, MultipleFetch) {
  size_t class_index = 5;
  std::vector<std::pair<void *, size_t>> batches;

  for (int i = 0; i < 5; ++i) {
    size_t actual_count = 0;
    void *start = cc_.FetchRange(class_index, 10, &actual_count);
    if (start != nullptr && actual_count > 0) {
      batches.emplace_back(start, actual_count);
    }
  }

  for (auto &batch : batches) {
    cc_.ReleaseRange(class_index, batch.first, batch.second);
  }
}

// 边界测试
TEST_F(CentralCacheTest, FetchZero) {
  size_t actual_count = 0;
  void *start = cc_.FetchRange(0, 0, &actual_count);
  // 请求 0 个可能返回 nullptr 或实际分配
}

TEST_F(CentralCacheTest, FetchLargeBatch) {
  size_t actual_count = 0;
  void *start = cc_.FetchRange(0, 1000, &actual_count);
  if (start != nullptr && actual_count > 0) {
    cc_.ReleaseRange(0, start, actual_count);
  }
}

// 单例测试
TEST_F(CentralCacheTest, SingletonConsistency) {
  CentralCache &cc1 = CentralCache::Instance();
  CentralCache &cc2 = CentralCache::Instance();
  EXPECT_EQ(&cc1, &cc2);
}

} // namespace
} // namespace zmalloc
