#include "zmalloc/zmalloc.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "zmalloc/internal/central_cache.h"
#include "zmalloc/internal/free_list.h"
#include "zmalloc/internal/transfer_cache.h"

namespace {

class MemoryTest : public ::testing::Test {
  protected:
    void SetUp() override { zmalloc::release_memory(); }
    void TearDown() override { zmalloc::release_memory(); }
};

TEST_F(MemoryTest, DrainReturnsRealObjectsFromAllSizeClassesToCentral) {
    auto &transfer = zmalloc::TransferCache::get_instance();
    size_t bytes = 0;
    // 使用中心层实际分配的对象，验证 drain 的所有权转交，不使用假指针。
    for (size_t size = 1; size <= zmalloc::MAX_BYTES;) {
        const auto &e = zmalloc::SizeClass::lookup(size);
        void *start = nullptr, *end = nullptr;
        const size_t got = zmalloc::CentralCache::get_instance().fetch_range_obj(
            start, end, 2, e.align_size, e.index);
        void *objects[2] = {start, got > 1 ? zmalloc::next_obj(start) : nullptr};
        ASSERT_EQ(transfer.insert_range(e.index, objects, got), got);
        bytes += got * e.align_size;
        size = e.align_size + 1;
    }
    EXPECT_EQ(transfer.cached_bytes(), bytes);
    EXPECT_EQ(transfer.drain(), bytes);
    EXPECT_EQ(transfer.cached_bytes(), 0u);
    EXPECT_EQ(transfer.drain(), 0u);
}

TEST_F(MemoryTest, FullReleaseDrainsCachesAndCanBeRepeatedAfterReuse) {
    const size_t count = 4096, size = 4096;
    std::vector<void *> objects(count);
    for (void *&ptr : objects) {
        ptr = zmalloc::zmalloc(size);
        std::memset(ptr, 0x5a, size);
    }
    for (void *ptr : objects) zmalloc::zfree(ptr);
    const auto before = zmalloc::memory_stats();
    EXPECT_GT(before.thread_cache_bytes, 0u);
    EXPECT_GT(before.transfer_cache_bytes, 0u);
    EXPECT_GE(before.page_cache_mapped_bytes, count * size);
    const size_t released = zmalloc::release_memory();
    const auto after = zmalloc::memory_stats();
    EXPECT_GE(released, count * size);
    EXPECT_EQ(after.thread_cache_bytes, 0u);
    EXPECT_EQ(after.transfer_cache_bytes, 0u);
    EXPECT_EQ(after.page_cache_free_bytes, after.page_cache_released_bytes);
    EXPECT_EQ(after.total_released_bytes - before.total_released_bytes, released);
    EXPECT_EQ(after.page_cache_mapped_bytes, before.page_cache_mapped_bytes);
    EXPECT_EQ(zmalloc::release_memory(), 0u);

    // 回收后地址仍可复用；重新触页再释放应允许再次建议回收。
    for (void *&ptr : objects) {
        ptr = zmalloc::zmalloc(size);
        std::memset(ptr, 0xa5, size);
    }
    for (void *ptr : objects) {
        EXPECT_EQ(static_cast<unsigned char *>(ptr)[size - 1], 0xa5);
        zmalloc::zfree(ptr);
    }
    EXPECT_GE(zmalloc::release_memory(), count * size);
    EXPECT_EQ(zmalloc::release_memory(), 0u);
}

TEST_F(MemoryTest, ReleaseKeepsLiveSmallAndMediumObjectsAndAccountsDirectMappings) {
    auto *small = static_cast<unsigned char *>(zmalloc::zmalloc(64));
    auto *medium = static_cast<unsigned char *>(zmalloc::zmalloc(512 * 1024));
    std::memset(small, 0x5a, 64);
    std::memset(medium, 0xa5, 512 * 1024);
    const auto before = zmalloc::memory_stats();
    void *large = zmalloc::zmalloc(2 * 1024 * 1024);
    EXPECT_EQ(zmalloc::memory_stats().page_cache_mapped_bytes,
              before.page_cache_mapped_bytes + 2 * 1024 * 1024);
    zmalloc::release_memory();
    for (size_t i = 0; i < 64; ++i) EXPECT_EQ(small[i], 0x5a);
    EXPECT_EQ(std::memcmp(medium, std::vector<unsigned char>(512 * 1024, 0xa5).data(),
                          512 * 1024), 0);
    zmalloc::zfree(large);
    EXPECT_EQ(zmalloc::memory_stats().page_cache_mapped_bytes,
              before.page_cache_mapped_bytes);
    auto &pc = zmalloc::PageCache::get_instance();
    EXPECT_NE(pc.try_map_cached_object_to_span(small), nullptr);
    EXPECT_NE(pc.try_map_cached_object_to_span(medium), nullptr);
    zmalloc::zfree(small);
    zmalloc::zfree(medium);
    EXPECT_GT(zmalloc::release_memory(), 0u);
}

TEST_F(MemoryTest, ReleaseDoesNotAccessAnotherThreadsLocalCache) {
    std::atomic<bool> ready(false), resume(false), intact(false);
    std::thread worker([&] {
        void *ptr = zmalloc::zmalloc(64);
        zmalloc::zfree(ptr);
        const size_t cached = zmalloc::get_thread_cache()->cached_bytes();
        ready.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
        intact.store(cached != 0 &&
                         zmalloc::get_thread_cache()->cached_bytes() == cached,
                     std::memory_order_relaxed);
        ptr = zmalloc::zmalloc(64);
        std::memset(ptr, 0x5a, 64);
        zmalloc::zfree(ptr);
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    zmalloc::release_memory();
    EXPECT_EQ(zmalloc::memory_stats().thread_cache_bytes, 0u);
    resume.store(true, std::memory_order_release);
    worker.join();
    EXPECT_TRUE(intact.load(std::memory_order_relaxed));
}

TEST_F(MemoryTest, ConcurrentReleasePreservesActiveObjects) {
    constexpr size_t workers = 4;
    const size_t sizes[] = {64, 4096, 8193, 262144, 300000, 2097152};
    std::atomic<size_t> ready(0), remaining(workers);
    std::atomic<bool> go(false), intact(true);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < workers; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t round = 0; round < 500; ++round) {
                unsigned char *objects[16];
                const size_t size = sizes[(round + t) % 6];
                for (auto &ptr : objects) {
                    ptr = static_cast<unsigned char *>(zmalloc::zmalloc(size));
                    std::memset(ptr, static_cast<int>(t + 1), size);
                }
                std::this_thread::yield();
                for (auto *ptr : objects) {
                    for (size_t i = 0; i < size; i += 4096) {
                        if (ptr[i] != t + 1) intact.store(false, std::memory_order_relaxed);
                    }
                    if (ptr[size - 1] != t + 1) intact.store(false, std::memory_order_relaxed);
                    zmalloc::zfree(ptr);
                }
            }
            remaining.fetch_sub(1, std::memory_order_release);
        });
    }
    // 两个回收者同时运行，验证共享排空和页回收之间的同步。
    for (size_t t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            while (remaining.load(std::memory_order_acquire) != 0) {
                zmalloc::release_memory();
                zmalloc::memory_stats();
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != workers + 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    EXPECT_TRUE(intact.load(std::memory_order_relaxed));
    zmalloc::release_memory();
    EXPECT_EQ(zmalloc::memory_stats().transfer_cache_bytes, 0u);
}

} // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
