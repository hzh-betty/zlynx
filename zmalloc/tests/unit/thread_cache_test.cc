#include <gtest/gtest.h>

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <thread>

#include "zmalloc/internal/size_class.h"
#include "zmalloc/internal/zmalloc_config.h"
// 先 include 基础头，避免 private->public 影响标准库头
#define private public
#include "zmalloc/internal/thread_cache.h"
#undef private

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
        zmalloc::CentralCache::get_instance().release_list_to_spans(
            objects[0], e.index);
    }
}

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
    zmalloc::ThreadCache cache;
    zmalloc::ThreadCache *tc = &cache;
    void SetUp() override { zmalloc::get_thread_cache()->cleanup(); }
    void TearDown() override {
        cache.cleanup();
        zmalloc::get_thread_cache()->cleanup();
    }
};
class ThreadCacheAllocFreeParamTest
    : public ThreadCacheTest, public ::testing::WithParamInterface<size_t> {};
class ThreadCacheTooLongParamTest
    : public ThreadCacheTest, public ::testing::WithParamInterface<size_t> {};

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

TEST_F(ThreadCacheTest, AccountingUsesAlignedSize) {
    void *p = tc->allocate(65);
    const size_t before = tc->cached_bytes();
    tc->deallocate(p, 65);
    EXPECT_EQ(tc->cached_bytes(), before + zmalloc::SizeClass::round_up(65));
    EXPECT_EQ(tc->allocate(65), p);
    EXPECT_EQ(tc->cached_bytes(), before);
    tc->deallocate(p, 65);
}

TEST_F(ThreadCacheTest, CleanupDrainsMoreThanOneBatchAndKeepsLiveObjects) {
    void *live = tc->allocate(64);
    auto *span = zmalloc::PageCache::get_instance().map_object_to_span(live);
    std::vector<void *> objects;
    for (size_t i = 0; i < 300; ++i) {
        objects.push_back(tc->allocate(64));
    }
    const size_t index = zmalloc::SizeClass::index(64);
    tc->free_lists_[index].max_size() = 1024;
    for (void *p : objects) {
        tc->deallocate(p, 64);
    }
    ASSERT_GT(tc->free_lists_[index].size(), 128u);
    tc->cleanup();
    EXPECT_EQ(tc->cached_bytes(), 0u);
    EXPECT_TRUE(tc->free_lists_[index].empty());
    DrainTransfer(64); // 共享缓存仍计入 span 外部引用，清空后只剩 live。
    EXPECT_EQ(span->use_count, 1u);
    static_cast<char *>(live)[63] = 42;
    tc->cleanup();
    tc->deallocate(live, 64);
}

TEST_F(ThreadCacheTest, LowWaterScavengesUnusedObjects) {
    std::vector<void *> objects;
    for (size_t i = 0; i < 200; ++i) {
        objects.push_back(tc->allocate(64));
    }
    const size_t index = zmalloc::SizeClass::index(64);
    tc->free_lists_[index].max_size() = 1024;
    for (void *p : objects) {
        tc->deallocate(p, 64);
    }
    tc->scavenge(); // 建立观察窗口。
    const size_t before = tc->cached_bytes();
    void *p = tc->allocate(64);
    tc->deallocate(p, 64);
    tc->scavenge();
    EXPECT_LT(tc->cached_bytes(), before);
    EXPECT_GT(tc->cached_bytes(), 0u);
}

TEST_F(ThreadCacheTest, ScavengeBatchesWithoutExceedingIdleLowWater) {
    const auto &e = zmalloc::SizeClass::lookup(4096);
    for (size_t idle : {10u, 24u}) {
        std::vector<void *> objects;
        for (size_t i = 0; i < 32; ++i) {
            objects.push_back(tc->allocate(e.align_size));
        }
        tc->cleanup();
        tc->free_lists_[e.index].max_size() = 64;
        for (size_t i = 0; i < idle; ++i) {
            tc->deallocate(objects[i], e.align_size);
        }
        tc->free_lists_[e.index].reset_low_water();
        tc->scavenge();
        EXPECT_EQ(tc->cached_bytes(),
                  (idle - std::min<size_t>(idle, e.num_move)) * e.align_size);
        // 回收仅针对闲置对象，调用方仍持有的对象必须保持可用。
        for (size_t i = idle; i < objects.size(); ++i) {
            static_cast<char *>(objects[i])[e.align_size - 1] = 42;
            tc->deallocate(objects[i], e.align_size);
        }
        tc->cleanup();
    }
}

TEST_F(ThreadCacheTest, MixedSizeBudgetTriggersScavenging) {
    // 不同规格共同超过预算，验证全线程预算而不只是单链表上限。
    for (size_t size = 8192; size <= zmalloc::MAX_BYTES; size += 8192) {
        void *p = tc->allocate(size);
        tc->deallocate(p, size);
    }
    EXPECT_LE(tc->cached_bytes(), 2 * zmalloc::ThreadCache::kCacheBudget);
    tc->cleanup();
    EXPECT_EQ(tc->cached_bytes(), 0u);
}

TEST_F(ThreadCacheTest, FourKiBBatchesRespectBudgetAndPreserveLiveSpan) {
    constexpr size_t size = 4096;
    const auto &e = zmalloc::SizeClass::lookup(size);
    DrainTransfer(size);
    auto *live = static_cast<unsigned char *>(tc->allocate(size));
    auto *span = zmalloc::PageCache::get_instance().map_object_to_span(live);
    live[0] = 0x5a;
    live[size - 1] = 0xa5;

    // 覆盖小于、等于及超过四批上限的工作集，反复触发补货和超限回收。
    for (size_t count : {64u, 256u, 512u}) {
        std::vector<void *> objects(count);
        for (size_t round = 0; round < 100; ++round) {
            std::unordered_set<void *> unique;
            for (void *&ptr : objects) {
                ptr = tc->allocate(size);
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
                tc->deallocate(ptr, size);
            }
            EXPECT_LE(tc->free_lists_[e.index].max_size(), 4u * e.num_move);
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
    tc->deallocate(live, size);
}

TEST_F(ThreadCacheTest, ProducerConsumerCapacityShrinks) {
    const auto &e = zmalloc::SizeClass::lookup(64);
    std::vector<void *> objects;
    for (size_t i = 0; i < 3000; ++i) {
        objects.push_back(tc->allocate(64));
    }
    const size_t peak = tc->free_lists_[e.index].max_size();
    EXPECT_LE(peak, 4u * e.num_move);
    for (void *p : objects) {
        tc->deallocate(p, 64);
    }
    EXPECT_LT(tc->free_lists_[e.index].max_size(), peak);
}

TEST_F(ThreadCacheTest, ShutdownBypassesCacheAndIsIdempotent) {
    void *p = tc->allocate(64);
    tc->deallocate(p, 64);
    tc->shutdown();
    for (int i = 0; i < 10; ++i) {
        p = tc->allocate(65);
        tc->deallocate(p, 65);
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
        auto *span = zmalloc::PageCache::get_instance().map_object_to_span(live);
        DrainTransfer(64);
        EXPECT_EQ(span->use_count, 1u);
        EXPECT_EQ(static_cast<char *>(live)[63], 42);
        zmalloc::zfree(live);
        zmalloc::get_thread_cache()->cleanup();
    }
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
        objects.push_back(tc->allocate(size));
    }
    std::unordered_set<void *> expected(objects.begin(), objects.end());
    tc->free_lists_[e.index].max_size() = 512;
    for (void *p : objects) {
        tc->deallocate(p, size);
    }
    // 传输容量仅容纳一批，超出容量的对象必须安全回退中央层。
    tc->release_batch(e.index, tc->free_lists_[e.index].size());
    auto &transfer = zmalloc::TransferCache::get_instance();
    EXPECT_TRUE(transfer.get_entry(e.index).full());
    const size_t available = transfer.get_entry(e.index).size();
    ASSERT_EQ(available, e.num_move);
    // 取走一个存活对象，确保消费者请求一整批时仍正确处理不足一批的命中。
    void *held = nullptr;
    ASSERT_EQ(transfer.remove_range(e.index, &held, 1), 1u);
    zmalloc::ThreadCache consumer;
    consumer.free_lists_[e.index].max_size() = e.num_move;
    void *first = consumer.allocate(size);
    EXPECT_EQ(expected.count(first), 1u);
    EXPECT_EQ(consumer.free_lists_[e.index].size(), available - 2);
    EXPECT_TRUE(transfer.get_entry(e.index).empty());
    consumer.deallocate(first, size);
    consumer.deallocate(held, size);
    consumer.cleanup();
    EXPECT_EQ(consumer.cached_bytes(), 0u);
    DrainTransfer(size);
}

TEST_F(ThreadCacheTest, TransferCachePartialHitKeepsSpanAlive) {
    const size_t size = 8192;
    const auto &e = zmalloc::SizeClass::lookup(size);
    DrainTransfer(size);
    void *p = tc->allocate(size);
    tc->cleanup();
    void *objects[] = {p};
    ASSERT_EQ(zmalloc::TransferCache::get_instance().insert_range(e.index,
                                                                  objects, 1),
              1u);
    zmalloc::ThreadCache consumer;
    consumer.free_lists_[e.index].max_size() = e.num_move;
    EXPECT_EQ(consumer.allocate(size), p);
    EXPECT_EQ(consumer.cached_bytes(), 0u);
    static_cast<char *>(p)[size - 1] = 42;
    EXPECT_EQ(
        zmalloc::PageCache::get_instance().map_object_to_span(p)->use_count,
        1u);
    consumer.deallocate(p, size);
    consumer.cleanup();
}

TEST_P(ThreadCacheAllocFreeParamTest, AllocTouchFreeBySize) {
    AllocTouchFree(tc, GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Sizes, ThreadCacheAllocFreeParamTest,
    ::testing::Values(1u, 2u, 7u, 8u, 9u, 15u, 16u, 17u, 24u, 31u, 32u, 33u,
                      48u, 63u, 64u, 65u, 80u, 95u, 96u, 97u, 112u, 127u,
                      128u, 129u, 192u, 255u, 256u, 257u, 384u, 511u, 512u,
                      513u, 768u, 1023u, 1024u, 1025u, 2047u, 2048u, 2049u,
                      4095u, 4096u, 4097u, 8191u, 8192u, 8193u, 16384u,
                      32768u, 65536u, 131072u, 200000u, 262144u));

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
