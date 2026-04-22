/**
 * @file transfer_cache_test.cc
 * @brief TransferCache 单元测试
 */

#define private public
#include "zmalloc/internal/transfer_cache.h"
#undef private
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <tuple>
#include <vector>

namespace zmalloc {
namespace {

class TransferCacheTest : public ::testing::Test {
  protected:
    void SetUp() override {}
    void TearDown() override {}
};
class TransferCacheEntryExactParamTest
    : public TransferCacheTest, public ::testing::WithParamInterface<size_t> {};
class TransferCacheEntryPartialParamTest
    : public TransferCacheTest,
      public ::testing::WithParamInterface<std::tuple<size_t, size_t>> {};
class TransferCacheEntryEmptyParamTest
    : public TransferCacheTest, public ::testing::WithParamInterface<size_t> {};
class TransferCacheManagerExactParamTest
    : public TransferCacheTest,
      public ::testing::WithParamInterface<std::tuple<size_t, size_t>> {};

static void FillUniquePtrs(void **out, size_t n, uintptr_t base) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = reinterpret_cast<void *>(base +
                                          static_cast<uintptr_t>((i + 1) * 16));
    }
}

static void InsertRemoveExactEntry(TransferCacheEntry &cache, size_t n,
                                   uintptr_t base) {
    std::vector<void *> in(n);
    FillUniquePtrs(in.data(), n, base);
    size_t inserted = cache.insert_range(in.data(), n);
    ASSERT_EQ(inserted, n);
    EXPECT_EQ(cache.size(), n);

    std::vector<void *> out(n, nullptr);
    size_t removed = cache.remove_range(out.data(), n);
    ASSERT_EQ(removed, n);
    EXPECT_TRUE(cache.empty());
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], in[i]);
    }
}

static void InsertRemoveExactManager(TransferCache &manager, size_t index,
                                     size_t n, uintptr_t base) {
    std::vector<void *> in(n);
    FillUniquePtrs(in.data(), n, base);
    size_t inserted = manager.insert_range(index, in.data(), n);
    ASSERT_EQ(inserted, n);
    std::vector<void *> out(n, nullptr);
    size_t removed = manager.remove_range(index, out.data(), n);
    ASSERT_EQ(removed, n);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], in[i]);
    }
}

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
            objs[i] = reinterpret_cast<void *>(
                static_cast<uintptr_t>(round * 100 + i));
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
        extra[i] =
            reinterpret_cast<void *>(static_cast<uintptr_t>(kMaxSlots + i));
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
            objs[i] = reinterpret_cast<void *>(
                static_cast<uintptr_t>(round * 32 + i));
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

TEST_F(TransferCacheTest, InsertRemoveZeroCountNoop) {
    TransferCacheEntry cache;
    void *dummy[1] = {reinterpret_cast<void *>(0x1)};
    EXPECT_EQ(cache.insert_range(dummy, 0), 0u);
    EXPECT_EQ(cache.remove_range(dummy, 0), 0u);
    EXPECT_TRUE(cache.empty());
}

TEST_F(TransferCacheTest, InsertRangeWrapAroundCopiesBothSegments) {
    TransferCacheEntry cache;
    cache.head_ = TransferCacheEntry::kMaxCacheSlots - 2;
    cache.tail_ = 0;
    cache.count_.store(0, std::memory_order_relaxed);

    void *objs[4];
    FillUniquePtrs(objs, 4, 0x4000u);
    size_t inserted = cache.insert_range(objs, 4);
    ASSERT_EQ(inserted, 4u);
    EXPECT_EQ(cache.size(), 4u);
    EXPECT_EQ(cache.slots_[TransferCacheEntry::kMaxCacheSlots - 2], objs[0]);
    EXPECT_EQ(cache.slots_[TransferCacheEntry::kMaxCacheSlots - 1], objs[1]);
    EXPECT_EQ(cache.slots_[0], objs[2]);
    EXPECT_EQ(cache.slots_[1], objs[3]);
}

TEST_F(TransferCacheTest, RemoveRangeWrapAroundCopiesBothSegments) {
    TransferCacheEntry cache;
    cache.head_ = 2;
    cache.tail_ = TransferCacheEntry::kMaxCacheSlots - 2;
    cache.count_.store(4, std::memory_order_relaxed);

    void *objs[4];
    FillUniquePtrs(objs, 4, 0x5000u);
    cache.slots_[TransferCacheEntry::kMaxCacheSlots - 2] = objs[0];
    cache.slots_[TransferCacheEntry::kMaxCacheSlots - 1] = objs[1];
    cache.slots_[0] = objs[2];
    cache.slots_[1] = objs[3];

    void *out[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t removed = cache.remove_range(out, 4);
    ASSERT_EQ(removed, 4u);
    EXPECT_TRUE(cache.empty());
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(out[i], objs[i]);
    }
}

TEST_F(TransferCacheTest, TryInsertRangeLockContentionReturnsFalse) {
    TransferCacheEntry cache;
    void *objs[1] = {reinterpret_cast<void *>(0x11)};
    size_t inserted = 0;

    cache.mtx_.lock();
    bool ok = cache.try_insert_range(objs, 1, inserted);
    cache.mtx_.unlock();

    EXPECT_FALSE(ok);
    EXPECT_EQ(inserted, 0u);
}

TEST_F(TransferCacheTest, TryRemoveRangeLockContentionReturnsFalse) {
    TransferCacheEntry cache;
    void *objs[1] = {reinterpret_cast<void *>(0x22)};
    ASSERT_EQ(cache.insert_range(objs, 1), 1u);
    size_t removed = 0;
    void *out[1] = {nullptr};

    cache.mtx_.lock();
    bool ok = cache.try_remove_range(out, 1, removed);
    cache.mtx_.unlock();

    EXPECT_FALSE(ok);
    EXPECT_EQ(removed, 0u);
}

TEST_F(TransferCacheTest, TryInsertRangeZeroAndFullPaths) {
    TransferCacheEntry cache;
    void *objs[1] = {reinterpret_cast<void *>(0x33)};
    size_t inserted = 123;

    EXPECT_TRUE(cache.try_insert_range(objs, 0, inserted));
    EXPECT_EQ(inserted, 0u);

    cache.count_.store(TransferCacheEntry::kMaxCacheSlots,
                       std::memory_order_relaxed);
    inserted = 123;
    EXPECT_TRUE(cache.try_insert_range(objs, 1, inserted));
    EXPECT_EQ(inserted, 0u);
}

TEST_F(TransferCacheTest, TryRemoveRangeZeroAndEmptyPaths) {
    TransferCacheEntry cache;
    void *out[2] = {nullptr, nullptr};
    size_t removed = 321;

    EXPECT_TRUE(cache.try_remove_range(out, 0, removed));
    EXPECT_EQ(removed, 0u);

    removed = 321;
    EXPECT_TRUE(cache.try_remove_range(out, 2, removed));
    EXPECT_EQ(removed, 0u);
}

TEST_F(TransferCacheTest, TryInsertAndTryRemoveSuccessPath) {
    TransferCacheEntry cache;
    void *objs[3];
    FillUniquePtrs(objs, 3, 0x6000u);
    size_t inserted = 0;
    ASSERT_TRUE(cache.try_insert_range(objs, 3, inserted));
    ASSERT_EQ(inserted, 3u);

    void *out[3] = {nullptr, nullptr, nullptr};
    size_t removed = 0;
    ASSERT_TRUE(cache.try_remove_range(out, 3, removed));
    ASSERT_EQ(removed, 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(out[i], objs[i]);
    }
}

TEST_F(TransferCacheTest, TryInsertRangeWrapAroundCopiesBothSegments) {
    TransferCacheEntry cache;
    cache.head_ = TransferCacheEntry::kMaxCacheSlots - 1;
    cache.tail_ = 0;
    cache.count_.store(0, std::memory_order_relaxed);

    void *objs[3];
    FillUniquePtrs(objs, 3, 0x6500u);
    size_t inserted = 0;
    ASSERT_TRUE(cache.try_insert_range(objs, 3, inserted));
    ASSERT_EQ(inserted, 3u);
    EXPECT_EQ(cache.slots_[TransferCacheEntry::kMaxCacheSlots - 1], objs[0]);
    EXPECT_EQ(cache.slots_[0], objs[1]);
    EXPECT_EQ(cache.slots_[1], objs[2]);
}

TEST_F(TransferCacheTest, TryRemoveRangeWrapAroundCopiesBothSegments) {
    TransferCacheEntry cache;
    cache.head_ = 1;
    cache.tail_ = TransferCacheEntry::kMaxCacheSlots - 1;
    cache.count_.store(3, std::memory_order_relaxed);

    void *objs[3];
    FillUniquePtrs(objs, 3, 0x6800u);
    cache.slots_[TransferCacheEntry::kMaxCacheSlots - 1] = objs[0];
    cache.slots_[0] = objs[1];
    cache.slots_[1] = objs[2];

    void *out[3] = {nullptr, nullptr, nullptr};
    size_t removed = 0;
    ASSERT_TRUE(cache.try_remove_range(out, 3, removed));
    ASSERT_EQ(removed, 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(out[i], objs[i]);
    }
}

TEST_F(TransferCacheTest, ManagerTryInsertAndTryRemove) {
    TransferCache &manager = TransferCache::get_instance();
    void *objs[4];
    FillUniquePtrs(objs, 4, 0x7000u);
    size_t inserted = 0;
    ASSERT_TRUE(manager.try_insert_range(9, objs, 4, inserted));
    ASSERT_EQ(inserted, 4u);

    void *out[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t removed = 0;
    ASSERT_TRUE(manager.try_remove_range(9, out, 4, removed));
    ASSERT_EQ(removed, 4u);
}

TEST_P(TransferCacheEntryExactParamTest, InsertRemoveExactCount) {
    const size_t n = GetParam();
    TransferCacheEntry cache;
    InsertRemoveExactEntry(cache, n, 0x1000u + static_cast<uintptr_t>(n));
}

INSTANTIATE_TEST_SUITE_P(
    Counts, TransferCacheEntryExactParamTest,
    ::testing::Values(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 15u, 16u, 31u,
                      32u, 63u, 64u, 127u, 128u));

TEST_P(TransferCacheEntryPartialParamTest, PartialRemoveKeepsRemainingCount) {
    const size_t inserted = std::get<0>(GetParam());
    const size_t requested = std::get<1>(GetParam());
    TransferCacheEntry cache;
    std::vector<void *> in(inserted);
    FillUniquePtrs(in.data(), inserted,
                   0x2000u + static_cast<uintptr_t>(inserted));
    ASSERT_EQ(cache.insert_range(in.data(), inserted), inserted);
    std::vector<void *> out(requested, nullptr);
    const size_t removed = cache.remove_range(out.data(), requested);
    ASSERT_EQ(removed, requested);
    EXPECT_EQ(cache.size(), inserted - requested);
    for (size_t i = 0; i < requested; ++i) {
        EXPECT_EQ(out[i], in[i]);
    }
}

INSTANTIATE_TEST_SUITE_P(
    InsertRemoveCounts, TransferCacheEntryPartialParamTest,
    ::testing::Values(std::make_tuple(8u, 1u), std::make_tuple(8u, 2u),
                      std::make_tuple(8u, 7u), std::make_tuple(16u, 1u),
                      std::make_tuple(16u, 8u), std::make_tuple(32u, 1u),
                      std::make_tuple(32u, 16u), std::make_tuple(64u, 1u),
                      std::make_tuple(64u, 32u),
                      std::make_tuple(128u, 64u)));

TEST_P(TransferCacheEntryEmptyParamTest, EmptyRemoveReturnsZero) {
    TransferCacheEntry cache;
    std::vector<void *> out(GetParam(), nullptr);
    EXPECT_EQ(cache.remove_range(out.data(), GetParam()), 0u);
    EXPECT_TRUE(cache.empty());
}

INSTANTIATE_TEST_SUITE_P(Counts, TransferCacheEntryEmptyParamTest,
                         ::testing::Values(1u, 2u, 8u, 64u));

TEST_P(TransferCacheManagerExactParamTest, InsertRemoveExactIndexAndCount) {
    const size_t index = std::get<0>(GetParam());
    const size_t n = std::get<1>(GetParam());
    TransferCache &manager = TransferCache::get_instance();
    InsertRemoveExactManager(
        manager, index, n, 0x3000u + static_cast<uintptr_t>(index * 1024 + n));
}

INSTANTIATE_TEST_SUITE_P(
    IndexCountPairs, TransferCacheManagerExactParamTest,
    ::testing::Values(std::make_tuple(0u, 1u), std::make_tuple(0u, 8u),
                      std::make_tuple(0u, 32u), std::make_tuple(1u, 1u),
                      std::make_tuple(1u, 8u), std::make_tuple(1u, 32u),
                      std::make_tuple(2u, 1u), std::make_tuple(2u, 8u),
                      std::make_tuple(2u, 32u), std::make_tuple(3u, 1u),
                      std::make_tuple(3u, 8u), std::make_tuple(3u, 32u),
                      std::make_tuple(7u, 1u), std::make_tuple(7u, 8u),
                      std::make_tuple(7u, 32u)));

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
