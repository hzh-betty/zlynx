#include "../support/cache_test_access.h"
#include "../support/thread_objects.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "zmalloc/internal/size_class.h"
#include "zmalloc/internal/thread_cache.h"
#include "zmalloc/internal/zmalloc_config.h"

#include "zmalloc/internal/central_cache.h"
#include "zmalloc/internal/transfer_cache.h"
#include "zmalloc/zmalloc.h"
#include <unordered_set>

namespace {

void DrainTransfer(size_t size) {
    const auto &e = zmalloc::SizeClass::lookup(size);
    void *objects[128];
    size_t count;
    while ((count = zmalloc::TransferCache::get_instance().remove_range(
                e.index, objects, 128)) != 0) {
        for (size_t i = 1; i < count; ++i) {
            zmalloc::next_obj(objects[i - 1]) = objects[i];
        }
        zmalloc::next_obj(objects[count - 1]) = nullptr;
        zmalloc::CentralCache::get_instance().release_list_to_spans(objects[0],
                                                                    e.index);
    }
}

static bool IsAligned(void *p, size_t align) {
    return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
}

static void AllocTouchFree(zmalloc::ThreadCache *tc, size_t size) {
    zmalloc::test::ThreadObjects owners(*tc);
    void *p = owners.allocate(size);
    ASSERT_NE(p, nullptr);

    // 只做非常轻量的触摸，避免越界。
    static_cast<unsigned char *>(p)[0] = 0xAB;
    owners.deallocate(p, size);
}

static void TriggerListTooLongOnce(zmalloc::ThreadCache *tc, size_t size,
                                   size_t new_max_size) {
    zmalloc::test::ThreadObjects owners(*tc);
    const size_t index = zmalloc::SizeClass::index_fast(size);
    size_t old = zmalloc::CacheTestAccess::lists(*tc)[index].max_size();
    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = new_max_size;

    // 申请 3 个并全部释放：在较小阈值下应触发回收。
    void *p1 = owners.allocate(size);
    void *p2 = owners.allocate(size);
    void *p3 = owners.allocate(size);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);
    owners.deallocate(p1, size);
    owners.deallocate(p2, size);
    owners.deallocate(p3, size);

    // 清理：取回两个再释放，避免把链表留得太长。
    void *q1 = owners.allocate(size);
    void *q2 = owners.allocate(size);
    ASSERT_NE(q1, nullptr);
    ASSERT_NE(q2, nullptr);
    owners.deallocate(q1, size);
    owners.deallocate(q2, size);

    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = old;
}

} // namespace

class ThreadCacheTest : public ::testing::Test {
  protected:
    zmalloc::ThreadCache cache;
    zmalloc::ThreadCache *tc = &cache;
    zmalloc::test::ThreadObjects owners{cache};

    void SetUp() override { zmalloc::get_thread_cache()->cleanup(); }

    void TearDown() override {
        owners.clear();
        cache.cleanup();
        zmalloc::get_thread_cache()->cleanup();
    }
};

class ThreadCacheAllocFreeParamTest
    : public ThreadCacheTest,
      public ::testing::WithParamInterface<size_t> {};

class ThreadCacheTooLongParamTest
    : public ThreadCacheTest,
      public ::testing::WithParamInterface<size_t> {};

TEST_F(ThreadCacheTest, AllocateReturnsNonNull) {
    void *p = owners.allocate(64);
    ASSERT_NE(p, nullptr);
    owners.deallocate(p, 64);
}

TEST_F(ThreadCacheTest, AllocateIsAligned) {
    const size_t req = 24;
    const size_t align = zmalloc::SizeClass::round_up_fast(req);
    void *p = owners.allocate(req);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(IsAligned(p, 8));
    EXPECT_TRUE(IsAligned(p, std::min<size_t>(align, 8))); // 小对象至少 8B
    owners.deallocate(p, req);
}

TEST_F(ThreadCacheTest, DeallocateThenAllocateReusesPointerLIFO) {
    void *p1 = owners.allocate(64);
    owners.deallocate(p1, 64);
    void *p2 = owners.allocate(64);
    EXPECT_EQ(p1, p2);
    owners.deallocate(p2, 64);
}

TEST_F(ThreadCacheTest, DifferentSizesDoNotShareSameFreeListIndexUsually) {
    void *a = owners.allocate(64);
    void *b = owners.allocate(128);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    owners.deallocate(a, 64);
    owners.deallocate(b, 128);
    SUCCEED();
}

TEST_F(ThreadCacheTest, CanHandleManyAllocFreeSmall) {
    std::vector<void *> v;
    for (int i = 0; i < 2000; ++i) {
        v.push_back(owners.allocate(64));
    }
    for (auto *p : v) {
        owners.deallocate(p, 64);
    }
    SUCCEED();
}

TEST_F(ThreadCacheTest, ListTooLongRecyclesWithoutCrash) {
    const size_t size = 64;
    const size_t index = zmalloc::SizeClass::index_fast(size);

    // 把阈值调小，确保触发 list_too_long。
    size_t old = zmalloc::CacheTestAccess::lists(*tc)[index].max_size();
    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = 2;

    void *p1 = owners.allocate(size);
    void *p2 = owners.allocate(size);
    void *p3 = owners.allocate(size);
    owners.deallocate(p1, size);
    owners.deallocate(p2, size);
    owners.deallocate(p3, size); // 应触发回收

    // 清理：再取几个并释放，避免污染其他测试
    void *q1 = owners.allocate(size);
    void *q2 = owners.allocate(size);
    owners.deallocate(q1, size);
    owners.deallocate(q2, size);

    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = old;
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
    void *a = owners.allocate(32);
    void *b = owners.allocate(64);
    void *c = owners.allocate(96);
    owners.deallocate(b, 64);
    owners.deallocate(a, 32);
    owners.deallocate(c, 96);
    SUCCEED();
}

TEST_F(ThreadCacheTest, TouchingMemoryDoesNotCrash) {
    char *p = static_cast<char *>(owners.allocate(128));
    ASSERT_NE(p, nullptr);
    p[0] = 1;
    p[127] = 2;
    owners.deallocate(p, 128);
}

TEST_F(ThreadCacheTest, AccountingUsesAlignedSize) {
    void *p = owners.allocate(65);
    const size_t before = tc->cached_bytes();
    owners.deallocate(p, 65);
    EXPECT_EQ(tc->cached_bytes(), before + zmalloc::SizeClass::round_up(65));
    EXPECT_EQ(owners.allocate(65), p);
    EXPECT_EQ(tc->cached_bytes(), before);
    owners.deallocate(p, 65);
}

TEST_F(ThreadCacheTest, CleanupDrainsMoreThanOneBatchAndKeepsLiveObjects) {
    void *live = owners.allocate(64);
    auto *span = zmalloc::PageCache::get_instance().map_object_to_span(live);
    std::vector<void *> objects;
    for (size_t i = 0; i < 300; ++i) {
        objects.push_back(owners.allocate(64));
    }
    const size_t index = zmalloc::SizeClass::index(64);
    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = 1024;
    for (void *p : objects) {
        owners.deallocate(p, 64);
    }
    ASSERT_GT(zmalloc::CacheTestAccess::lists(*tc)[index].size(), 128u);
    tc->cleanup();
    EXPECT_EQ(tc->cached_bytes(), 0u);
    EXPECT_TRUE(zmalloc::CacheTestAccess::lists(*tc)[index].empty());
    DrainTransfer(64); // 共享缓存仍计入 span 外部引用，清空后只剩 live。
    EXPECT_EQ(span->use_count, 1u);
    static_cast<char *>(live)[63] = 42;
    tc->cleanup();
    owners.deallocate(live, 64);
}

TEST_F(ThreadCacheTest, LowWaterScavengesUnusedObjects) {
    std::vector<void *> objects;
    for (size_t i = 0; i < 200; ++i) {
        objects.push_back(owners.allocate(64));
    }
    const size_t index = zmalloc::SizeClass::index(64);
    zmalloc::CacheTestAccess::lists(*tc)[index].max_size() = 1024;
    for (void *p : objects) {
        owners.deallocate(p, 64);
    }
    zmalloc::CacheTestAccess::scavenge(*tc); // 建立观察窗口。
    const size_t before = tc->cached_bytes();
    void *p = owners.allocate(64);
    owners.deallocate(p, 64);
    zmalloc::CacheTestAccess::scavenge(*tc);
    EXPECT_LT(tc->cached_bytes(), before);
    EXPECT_GT(tc->cached_bytes(), 0u);
}

TEST_F(ThreadCacheTest, ScavengeBatchesWithoutExceedingIdleLowWater) {
    const auto &e = zmalloc::SizeClass::lookup(4096);
    for (size_t idle : {10u, 24u}) {
        std::vector<void *> objects;
        for (size_t i = 0; i < 32; ++i) {
            objects.push_back(owners.allocate(e.align_size));
        }
        tc->cleanup();
        zmalloc::CacheTestAccess::lists(*tc)[e.index].max_size() = 64;
        for (size_t i = 0; i < idle; ++i) {
            owners.deallocate(objects[i], e.align_size);
        }
        zmalloc::CacheTestAccess::lists(*tc)[e.index].reset_low_water();
        zmalloc::CacheTestAccess::scavenge(*tc);
        EXPECT_EQ(tc->cached_bytes(),
                  (idle - std::min<size_t>(idle, e.num_move)) * e.align_size);

        // 回收仅针对闲置对象，调用方仍持有的对象必须保持可用。
        for (size_t i = idle; i < objects.size(); ++i) {
            static_cast<char *>(objects[i])[e.align_size - 1] = 42;
            owners.deallocate(objects[i], e.align_size);
        }
        tc->cleanup();
    }
}

TEST_F(ThreadCacheTest, MixedSizeBudgetTriggersScavenging) {
    // 不同规格共同超过预算，验证全线程预算而不只是单链表上限。
    for (size_t size = 8192; size <= zmalloc::MAX_BYTES; size += 8192) {
        void *p = owners.allocate(size);
        owners.deallocate(p, size);
    }
    EXPECT_LE(tc->cached_bytes(), 2 * zmalloc::ThreadCache::kCacheBudget);
    tc->cleanup();
    EXPECT_EQ(tc->cached_bytes(), 0u);
}

TEST_F(ThreadCacheTest, FourKiBBatchesRespectBudgetAndPreserveLiveSpan) {
    constexpr size_t size = 4096;
    const auto &e = zmalloc::SizeClass::lookup(size);
    DrainTransfer(size);
    auto *live = static_cast<unsigned char *>(owners.allocate(size));
    auto *span = zmalloc::PageCache::get_instance().map_object_to_span(live);
    live[0] = 0x5a;
    live[size - 1] = 0xa5;

    // 覆盖小于、等于及超过四批上限的工作集，反复触发补货和超限回收。
    for (size_t count : {64u, 256u, 512u}) {
        std::vector<void *> objects(count);
        for (size_t round = 0; round < 100; ++round) {
            std::unordered_set<void *> unique;
            for (void *&ptr : objects) {
                ptr = owners.allocate(size);
                ASSERT_NE(ptr, nullptr);
                ASSERT_NE(ptr, live);
                ASSERT_TRUE(unique.insert(ptr).second);
                auto *bytes = static_cast<unsigned char *>(ptr);
                bytes[0] = 0x3c;
                bytes[size - 1] = 0xc3;
            }
            for (void *ptr : objects) {
                auto *bytes = static_cast<unsigned char *>(ptr);
                EXPECT_EQ(bytes[0], 0x3c);
                EXPECT_EQ(bytes[size - 1], 0xc3);
                owners.deallocate(ptr, size);
            }
            EXPECT_LE(zmalloc::CacheTestAccess::lists(*tc)[e.index].max_size(),
                      4u * e.num_move);
            EXPECT_LE(tc->cached_bytes(), zmalloc::ThreadCache::kCacheBudget);
            EXPECT_EQ(live[0], 0x5a);
            EXPECT_EQ(live[size - 1], 0xa5);
        }
        tc->cleanup();
        DrainTransfer(size);
        EXPECT_EQ(tc->cached_bytes(), 0u);
        EXPECT_EQ(span->use_count, 1u);
        EXPECT_EQ(live[0], 0x5a);
        EXPECT_EQ(live[size - 1], 0xa5);
    }
    owners.deallocate(live, size);
}

TEST_F(ThreadCacheTest, ProducerConsumerCapacityShrinks) {
    const auto &e = zmalloc::SizeClass::lookup(64);
    std::vector<void *> objects;
    for (size_t i = 0; i < 3000; ++i) {
        objects.push_back(owners.allocate(64));
    }
    const size_t peak =
        zmalloc::CacheTestAccess::lists(*tc)[e.index].max_size();
    EXPECT_LE(peak, 4u * e.num_move);
    for (void *p : objects) {
        owners.deallocate(p, 64);
    }
    EXPECT_LT(zmalloc::CacheTestAccess::lists(*tc)[e.index].max_size(), peak);
}

TEST_F(ThreadCacheTest, ShutdownBypassesCacheAndIsIdempotent) {
    void *p = owners.allocate(64);
    owners.deallocate(p, 64);
    tc->shutdown();
    for (int i = 0; i < 10; ++i) {
        p = owners.allocate(65);
        owners.deallocate(p, 65);
        EXPECT_EQ(tc->cached_bytes(), 0u);
    }
    tc->shutdown();
}

TEST_F(ThreadCacheTest, ThreadExitReturnsObjectsButPreservesLiveAllocation) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        void *live = nullptr;
        std::thread worker([&] {
            live = zmalloc::zmalloc(64);
            static_cast<char *>(live)[63] = 42;
            std::vector<void *> objects;
            for (int i = 0; i < 300; ++i) {
                objects.push_back(zmalloc::zmalloc(64));
            }
            for (void *p : objects) {
                zmalloc::zfree(p);
            }
        });
        worker.join();
        auto *span =
            zmalloc::PageCache::get_instance().map_object_to_span(live);
        DrainTransfer(64);
        EXPECT_EQ(span->use_count, 1u);
        EXPECT_EQ(static_cast<char *>(live)[63], 42);
        zmalloc::zfree(live);
        zmalloc::get_thread_cache()->cleanup();
    }
}

TEST_F(ThreadCacheTest, FastAccessInitializesEachThreadAndKeepsShutdownState) {
    std::thread workers[4];
    zmalloc::ThreadCache *identities[4] = {};
    for (size_t i = 0; i < 4; ++i) {
        workers[i] = std::thread([&, i] {
            auto *cache = zmalloc::internal::get_thread_cache_fast();
            identities[i] = cache;
            EXPECT_EQ(cache, zmalloc::get_thread_cache());
            void *ptr = zmalloc::zmalloc(64);
            zmalloc::zfree(ptr);
            EXPECT_GT(cache->cached_bytes(), 0u);
            cache->shutdown();
            EXPECT_EQ(cache, zmalloc::internal::get_thread_cache_fast());
            ptr = zmalloc::zmalloc(64);
            zmalloc::zfree(ptr);
            EXPECT_EQ(cache->cached_bytes(), 0u);
        });
    }
    for (auto &worker : workers)
        worker.join();
    for (auto *identity : identities)
        EXPECT_NE(identity, nullptr);
}

TEST_F(ThreadCacheTest, LaterTlsDestructorCanAllocateAndFree) {
    bool finished = false;
    std::thread worker([&] {
        struct LateCleanup {
            bool *finished;
            void *live = nullptr;
            ~LateCleanup() {
                zmalloc::zfree(live);
                void *p = zmalloc::zmalloc(128);
                zmalloc::zfree(p);
                *finished = zmalloc::get_thread_cache()->cached_bytes() == 0;
            }
        };

        // 先注册，后析构：确保运行在 ThreadCache 的退出钩子之后。
        thread_local LateCleanup late{&finished};
        late.live = zmalloc::zmalloc(64);
    });
    worker.join();
    EXPECT_TRUE(finished);
}

TEST_F(ThreadCacheTest, TransferCacheReusesPartialBatchAndFullCacheFallsBack) {
    const size_t size = 4096;
    const auto &e = zmalloc::SizeClass::lookup(size);
    DrainTransfer(size);
    std::vector<void *> objects;
    for (size_t i = 0; i < 256; ++i) {
        objects.push_back(owners.allocate(size));
    }
    std::unordered_set<void *> expected(objects.begin(), objects.end());
    zmalloc::CacheTestAccess::lists(*tc)[e.index].max_size() = 512;
    for (void *p : objects) {
        owners.deallocate(p, size);
    }

    // 传输容量仅容纳一批，超出容量的对象必须安全回退中央层。
    zmalloc::CacheTestAccess::release_batch(
        *tc, e.index, zmalloc::CacheTestAccess::lists(*tc)[e.index].size());
    auto &transfer = zmalloc::TransferCache::get_instance();
    EXPECT_TRUE(transfer.get_entry(e.index).full());
    const size_t available = transfer.get_entry(e.index).size();
    ASSERT_EQ(available, e.num_move);

    // 取走一个存活对象，确保消费者请求一整批时仍正确处理不足一批的命中。
    zmalloc::ThreadCache consumer;
    zmalloc::test::ThreadObjects consumer_objects(consumer);
    void *held = nullptr;
    const size_t taken = transfer.remove_range(e.index, &held, 1);
    if (taken == 1)
        consumer_objects.adopt(held, size);
    ASSERT_EQ(taken, 1u);
    zmalloc::CacheTestAccess::lists(consumer)[e.index].max_size() = e.num_move;
    void *first = consumer_objects.allocate(size);
    EXPECT_EQ(expected.count(first), 1u);
    EXPECT_EQ(zmalloc::CacheTestAccess::lists(consumer)[e.index].size(),
              available - 2);
    EXPECT_TRUE(transfer.get_entry(e.index).empty());
    consumer_objects.deallocate(first, size);
    consumer_objects.deallocate(held, size);
    consumer.cleanup();
    EXPECT_EQ(consumer.cached_bytes(), 0u);
    DrainTransfer(size);
}

TEST_F(ThreadCacheTest, TransferCachePartialHitKeepsSpanAlive) {
    const size_t size = 8192;
    const auto &e = zmalloc::SizeClass::lookup(size);
    DrainTransfer(size);
    void *p = owners.allocate(size);
    tc->cleanup();
    void *objects[] = {p};
    const size_t inserted = zmalloc::TransferCache::get_instance().insert_range(
        e.index, objects, 1);
    if (inserted == 1)
        owners.release(p);
    ASSERT_EQ(inserted, 1u);
    zmalloc::ThreadCache consumer;
    zmalloc::test::ThreadObjects consumer_objects(consumer);
    zmalloc::CacheTestAccess::lists(consumer)[e.index].max_size() = e.num_move;
    EXPECT_EQ(consumer_objects.allocate(size), p);
    EXPECT_EQ(consumer.cached_bytes(), 0u);
    static_cast<char *>(p)[size - 1] = 42;
    EXPECT_EQ(
        zmalloc::PageCache::get_instance().map_object_to_span(p)->use_count,
        1u);
    consumer_objects.deallocate(p, size);
    consumer.cleanup();
}

TEST_P(ThreadCacheAllocFreeParamTest, AllocTouchFreeBySize) {
    AllocTouchFree(tc, GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, ThreadCacheAllocFreeParamTest,
    ::testing::Values(1u, 2u, 7u, 8u, 9u, 15u, 16u, 17u, 24u, 31u, 32u, 33u,
                      48u, 63u, 64u, 65u, 80u, 95u, 96u, 97u, 112u, 127u, 128u,
                      129u, 192u, 255u, 256u, 257u, 384u, 511u, 512u, 513u,
                      768u, 1023u, 1024u, 1025u, 2047u, 2048u, 2049u, 4095u,
                      4096u, 4097u, 8191u, 8192u, 8193u, 16384u, 32768u, 65536u,
                      131072u, 200000u, 262144u));

TEST_P(ThreadCacheTooLongParamTest, ListTooLongTriggerBySize) {
    TriggerListTooLongOnce(tc, GetParam(), 1);
}

INSTANTIATE_TEST_SUITE_P(Sizes, ThreadCacheTooLongParamTest,
                         ::testing::Values(32u, 64u, 128u, 256u, 512u, 1024u,
                                           4096u, 8192u, 65536u, 131072u));

int main(int argc, char **argv) {
    // 主线程 TLS 清理后仍需支持静态退出阶段的分配和释放。
    if (std::atexit([] {
            void *small = zmalloc::zmalloc(64);
            void *large = zmalloc::zmalloc(zmalloc::MAX_BYTES + 16);
            static_cast<char *>(small)[63] = 42;
            static_cast<char *>(large)[zmalloc::MAX_BYTES] = 42;
            zmalloc::zfree(small);
            zmalloc::zfree(large);
            if (zmalloc::get_thread_cache()->cached_bytes() != 0) {
                std::abort();
            }
        }) != 0) {
        return 1;
    }
    (void)zmalloc::get_thread_cache();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
