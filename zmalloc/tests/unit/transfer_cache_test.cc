/**
 * @file transfer_cache_test.cc
 * @brief TransferCache 单元测试
 */

#include "transfer_cache.h"
#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace zmalloc {
namespace {

class TransferCacheTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// 基本插入和获取测试
TEST_F(TransferCacheTest, BasicInsertRemove) {
  TransferCacheEntry cache;

  void *objs[10];
  for (int i = 0; i < 10; ++i) {
    objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }

  size_t inserted = cache.insert_range(objs, 10);
  EXPECT_EQ(inserted, 10u);
  EXPECT_EQ(cache.size(), 10u);
  EXPECT_FALSE(cache.empty());

  void *batch[10];
  size_t removed = cache.remove_range(batch, 10);
  EXPECT_EQ(removed, 10u);
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_TRUE(cache.empty());

  // 验证取出的对象正确
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(batch[i], objs[i]);
  }
}

// 空缓存测试
TEST_F(TransferCacheTest, EmptyCache) {
  TransferCacheEntry cache;
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(cache.size(), 0u);

  void *batch[10];
  size_t removed = cache.remove_range(batch, 10);
  EXPECT_EQ(removed, 0u);
}

// 部分获取测试
TEST_F(TransferCacheTest, PartialRemove) {
  TransferCacheEntry cache;

  void *objs[20];
  for (int i = 0; i < 20; ++i) {
    objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }

  cache.insert_range(objs, 20);
  EXPECT_EQ(cache.size(), 20u);

  void *batch[5];
  size_t removed = cache.remove_range(batch, 5);
  EXPECT_EQ(removed, 5u);
  EXPECT_EQ(cache.size(), 15u);
}

// 多次插入获取测试
TEST_F(TransferCacheTest, MultipleInsertRemove) {
  TransferCacheEntry cache;

  for (int round = 0; round < 10; ++round) {
    void *objs[50];
    for (int i = 0; i < 50; ++i) {
      objs[i] =
          reinterpret_cast<void *>(static_cast<uintptr_t>(round * 100 + i));
    }
    size_t inserted = cache.insert_range(objs, 50);
    EXPECT_EQ(inserted, 50u);

    void *batch[50];
    size_t removed = cache.remove_range(batch, 50);
    EXPECT_EQ(removed, 50u);
    EXPECT_TRUE(cache.empty());
  }
}

// TransferCache 管理器测试
TEST_F(TransferCacheTest, ManagerBasic) {
  TransferCache &manager = TransferCache::get_instance();

  void *objs[10];
  for (int i = 0; i < 10; ++i) {
    objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }

  // size class 0
  size_t inserted = manager.insert_range(0, objs, 10);
  EXPECT_EQ(inserted, 10u);

  void *batch[10];
  size_t removed = manager.remove_range(0, batch, 10);
  EXPECT_EQ(removed, 10u);
}

// 不同 size class 独立测试
TEST_F(TransferCacheTest, IndependentSizeClasses) {
  TransferCache &manager = TransferCache::get_instance();

  void *objs1[5];
  void *objs2[5];
  for (int i = 0; i < 5; ++i) {
    objs1[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 100));
    objs2[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 200));
  }

  manager.insert_range(1, objs1, 5);
  manager.insert_range(2, objs2, 5);

  void *batch1[5];
  void *batch2[5];
  size_t r1 = manager.remove_range(1, batch1, 5);
  size_t r2 = manager.remove_range(2, batch2, 5);

  EXPECT_EQ(r1, 5u);
  EXPECT_EQ(r2, 5u);

  // 验证数据独立
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(batch1[i], objs1[i]);
    EXPECT_EQ(batch2[i], objs2[i]);
  }
}

// 缓存满时插入测试
TEST_F(TransferCacheTest, FullCacheInsert) {
  TransferCacheEntry cache;
  constexpr size_t kMaxSlots = TransferCacheEntry::kMaxCacheSlots;

  // 填满缓存
  std::vector<void *> all_objs(kMaxSlots);
  for (size_t i = 0; i < kMaxSlots; ++i) {
    all_objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }

  size_t inserted = cache.insert_range(all_objs.data(), kMaxSlots);
  EXPECT_EQ(inserted, kMaxSlots);
  EXPECT_TRUE(cache.full());

  // 尝试再插入应该返回 0
  void *extra[10];
  for (int i = 0; i < 10; ++i) {
    extra[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(kMaxSlots + i));
  }
  inserted = cache.insert_range(extra, 10);
  EXPECT_EQ(inserted, 0u);
}

// 环形缓冲区正确性测试
TEST_F(TransferCacheTest, RingBufferCorrectness) {
  TransferCacheEntry cache;

  // 多轮插入和取出，验证环形缓冲区工作正常
  for (int round = 0; round < 100; ++round) {
    void *objs[32];
    for (int i = 0; i < 32; ++i) {
      objs[i] =
          reinterpret_cast<void *>(static_cast<uintptr_t>(round * 32 + i));
    }

    cache.insert_range(objs, 32);

    void *batch[32];
    size_t removed = cache.remove_range(batch, 32);
    EXPECT_EQ(removed, 32u);

    // 验证 FIFO 顺序
    for (int i = 0; i < 32; ++i) {
      EXPECT_EQ(batch[i], objs[i]);
    }
  }
}

// 边界条件：请求比缓存中少
TEST_F(TransferCacheTest, RequestLessThanAvailable) {
  TransferCacheEntry cache;

  void *objs[100];
  for (int i = 0; i < 100; ++i) {
    objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i));
  }

  cache.insert_range(objs, 100);

  void *batch[10];
  size_t removed = cache.remove_range(batch, 10);
  EXPECT_EQ(removed, 10u);
  EXPECT_EQ(cache.size(), 90u);
}

// 边界条件：请求比缓存中多
TEST_F(TransferCacheTest, RequestMoreThanAvailable) {
  TransferCacheEntry cache;

  void *objs[5];
  for (int i = 0; i < 5; ++i) {
    objs[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }

  cache.insert_range(objs, 5);

  void *batch[20];
  size_t removed = cache.remove_range(batch, 20);
  EXPECT_EQ(removed, 5u);
  EXPECT_TRUE(cache.empty());
}

// 并发插入测试
TEST_F(TransferCacheTest, ConcurrentInsert) {
  TransferCacheEntry cache;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        void *objs[8];
        for (int j = 0; j < 8; ++j) {
          objs[j] = reinterpret_cast<void *>(
              static_cast<uintptr_t>(t * 10000 + i * 100 + j));
        }
        cache.insert_range(objs, 8);
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  // 验证插入的数量（可能部分被拒绝）
  EXPECT_LE(cache.size(), TransferCacheEntry::kMaxCacheSlots);
}

// 并发插入和取出测试
TEST_F(TransferCacheTest, ConcurrentInsertRemove) {
  TransferCacheEntry cache;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 100;

  std::atomic<size_t> total_inserted{0};
  std::atomic<size_t> total_removed{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, &total_inserted, &total_removed, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        if (i % 2 == 0) {
          void *objs[4];
          for (int j = 0; j < 4; ++j) {
            objs[j] = reinterpret_cast<void *>(
                static_cast<uintptr_t>(t * 1000 + i * 10 + j));
          }
          total_inserted += cache.insert_range(objs, 4);
        } else {
          void *batch[4];
          total_removed += cache.remove_range(batch, 4);
        }
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  // 验证不变量：插入 - 取出 = 剩余
  EXPECT_EQ(cache.size(), total_inserted.load() - total_removed.load());
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
