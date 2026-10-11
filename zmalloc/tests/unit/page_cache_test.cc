#include "../support/cache_test_access.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "zmalloc/internal/span_list.h"
#include "zmalloc/internal/system_alloc.h"
#include "zmalloc/internal/zmalloc_config.h"

#include "zmalloc/internal/page_cache.h"

namespace {

static bool ContainsSpan(zmalloc::SpanList &list, zmalloc::Span *target) {
    for (zmalloc::Span *it = list.begin(); it != list.end(); it = it->next) {
        if (it == target) {
            return true;
        }
    }
    return false;
}

} // namespace

class PageCacheTest : public ::testing::Test {
  protected:
    zmalloc::PageCache &pc = zmalloc::PageCache::get_instance();
};
class PageCacheSmallSpanParamTest
    : public PageCacheTest, public ::testing::WithParamInterface<size_t> {};
class PageCacheLargeSpanParamTest
    : public PageCacheTest, public ::testing::WithParamInterface<size_t> {};

TEST_F(PageCacheTest, ReleasedStateSurvivesSplitAndResetsAfterReuseAndMerge) {
    // 独立实例保证测试确定地命中切分路径；所有访问仍遵守页锁约定。
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    auto *whole = span_owners.allocate(zmalloc::NPAGES - 1);
    auto *address = reinterpret_cast<unsigned char *>(whole->page_id << zmalloc::PAGE_SHIFT);
    const size_t bytes = whole->n * zmalloc::PAGE_SIZE;
    std::memset(address, 0x5a, bytes);
    span_owners.release(whole);
    EXPECT_EQ(cache.release_free_pages(), bytes);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    EXPECT_EQ(cache.statistics().released_bytes, bytes);

    auto *part = span_owners.allocate(32);
    EXPECT_FALSE(part->is_released);
    EXPECT_EQ(cache.statistics().released_bytes, bytes - 32 * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), part);
    std::memset(address, 0xa5, 32 * zmalloc::PAGE_SIZE);
    EXPECT_EQ(address[32 * zmalloc::PAGE_SIZE - 1], 0xa5);

    span_owners.release(part);
    EXPECT_EQ(cache.statistics().released_bytes, 0u);
    EXPECT_EQ(cache.release_free_pages(), bytes);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    whole = span_owners.allocate(zmalloc::NPAGES - 1);
    EXPECT_FALSE(whole->is_released);
    EXPECT_EQ(cache.statistics().released_bytes, 0u);
    EXPECT_EQ(address[0], 0);
    EXPECT_EQ(address[bytes - 1], 0);
    span_owners.release(whole);
    EXPECT_EQ(cache.release_free_pages(), bytes);
}

static void NewSpanCheckMappingAndRelease(zmalloc::PageCache &pc, size_t k) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(k);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->n, k);
    ASSERT_TRUE(span->is_use);

    const zmalloc::PageId page_id = span->page_id;
    // 小 span：要求每一页都建立映射。
    for (size_t i = 0; i < k; ++i) {
        auto *m = zmalloc::CacheTestAccess::decode(
            zmalloc::CacheTestAccess::page_map(pc).get(page_id + i));
        ASSERT_EQ(m, span);
        void *addr =
            reinterpret_cast<void *>((page_id + i) << zmalloc::PAGE_SHIFT);
        ASSERT_EQ(pc.map_object_to_span(addr), span);
    }

    span->is_use = true;
    span_owners.release(span);

    // release 后 span 指针可能失效；只验证起始页仍能定位到一个空闲 span。
    auto *a = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id));
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->is_use);

    auto *b = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id + k - 1));
    if (b != nullptr) {
        EXPECT_EQ(a, b);
        EXPECT_FALSE(b->is_use);
    }
}

static void NewLargeSpanAllocAndFree(zmalloc::PageCache &pc, size_t k) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(k);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->n, k);
    ASSERT_TRUE(span->is_use);
    span_owners.release(span);
}

TEST_F(PageCacheTest, NewSpanSmallBasicFields) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(1);
    ASSERT_NE(span, nullptr);
    EXPECT_GT(span->n, 0u);
    EXPECT_NE(span->page_id, 0u);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanStart) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(2);
    ASSERT_NE(span, nullptr);
    void *p = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanMiddle) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(4);
    ASSERT_NE(span, nullptr);
    void *p =
        reinterpret_cast<void *>((span->page_id + 2) << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, ReleaseSpanMarksNotInUse) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(3);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    const size_t page_id = span->page_id;
    const size_t n = span->n;
    span_owners.release(span);
    // release 之后 span 指针可能被合并/回收，不能再直接访问。
    auto *mapped = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id));
    ASSERT_NE(mapped, nullptr);
    EXPECT_FALSE(mapped->is_use);
    // 原 span 的结束页若仍是边界页，则也应指向同一个空闲 span。
    auto *mapped_end = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id + n - 1));
    if (mapped_end != nullptr) {
        EXPECT_EQ(mapped_end, mapped);
    }
}

TEST_F(PageCacheTest, ReleaseThenNewSpanCanReuseBucket) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span1 = span_owners.allocate(1);
    ASSERT_NE(span1, nullptr);
    const size_t page_id = span1->page_id;
    const size_t n = span1->n;
    span1->is_use = true;
    span_owners.release(span1);

    // release 后原 span1 指针可能失效；只验证映射仍可用。
    auto *mapped = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id));
    ASSERT_NE(mapped, nullptr);
    auto *mapped_end = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id + n - 1));
    ASSERT_NE(mapped_end, nullptr);
    EXPECT_EQ(mapped, mapped_end);
    EXPECT_FALSE(mapped->is_use);

    // 再次申请同尺寸 span 不应崩溃。
    zmalloc::Span *span2 = span_owners.allocate(1);
    ASSERT_NE(span2, nullptr);
}

TEST_F(PageCacheTest, LargeSpanGoesToSystemPath) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    const size_t k = zmalloc::NPAGES + 10;
    zmalloc::Span *span = span_owners.allocate(k);
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->n, k);
    EXPECT_TRUE(span->is_use);
    span_owners.release(span);
}

TEST_F(PageCacheTest, IdSpanMapHasStartAndEndForFreeSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(8);
    ASSERT_NE(span, nullptr);
    const size_t page_id = span->page_id;
    const size_t n = span->n;
    span->is_use = true;
    span_owners.release(span);

    // free span 只保证首尾页映射存在（用于合并）。
    auto *a = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id));
    auto *b = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id + n - 1));
    ASSERT_NE(a, nullptr);
    if (b != nullptr) {
        EXPECT_EQ(a, b);
    }
    EXPECT_FALSE(a->is_use);
}

TEST_F(PageCacheTest, ReleaseClearsInteriorMappingsForFreeSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(8);
    ASSERT_NE(span, nullptr);
    ASSERT_GT(span->n, 2u);

    const size_t middle_page = span->page_id + 3;
    span->is_use = true;
    span_owners.release(span);

    EXPECT_EQ(zmalloc::CacheTestAccess::page_map(pc).get(middle_page), nullptr);
}

TEST_F(PageCacheTest, ReusedSpanResetsAccountingState) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(4);
    ASSERT_NE(span, nullptr);

    span->obj_size = 160;
    span->use_count = 7;
    span->free_list = reinterpret_cast<void *>(0x1);
    span->is_use = true;
    const size_t page_id = span->page_id;
    span_owners.release(span);

    zmalloc::Span *reused = span_owners.allocate(4);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->page_id, page_id);
    EXPECT_EQ(reused->obj_size, 0u);
    EXPECT_EQ(reused->use_count, 0u);
    EXPECT_EQ(reused->free_list, nullptr);
}

TEST_F(PageCacheTest, MergeClearsOldNeighborBoundaryMappings) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    auto *left = span_owners.allocate(2);
    auto *middle = span_owners.allocate(2);
    auto *right = span_owners.allocate(2);
    auto *remainder = span_owners.allocate(zmalloc::NPAGES - 1 - 6);
    ASSERT_NE(remainder, nullptr);
    const auto base = left->page_id;
    ASSERT_EQ(middle->page_id, base + 2);
    ASSERT_EQ(right->page_id, base + 4);
    span_owners.release(left);
    span_owners.release(right);
    span_owners.release(middle);
    auto *merged = cache.try_map_object_to_span(
        reinterpret_cast<void *>(base << zmalloc::PAGE_SHIFT));
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->page_id, base);
    EXPECT_EQ(merged->n, 6u);
    EXPECT_FALSE(merged->is_use);
    EXPECT_EQ(merged->obj_size, 0u);
    EXPECT_EQ(merged->use_count, 0u);
    EXPECT_EQ(merged->free_list, nullptr);
    EXPECT_EQ(zmalloc::CacheTestAccess::page_map(cache).get(base), merged);
    EXPECT_EQ(zmalloc::CacheTestAccess::page_map(cache).get(base + 5), merged);
    for (size_t i = 1; i < 5; ++i)
        EXPECT_EQ(zmalloc::CacheTestAccess::page_map(cache).get(base + i),
                  nullptr);
}

TEST_F(PageCacheTest, NewSpanEstablishesAllPagesMapForSmallK) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(3);
    ASSERT_NE(span, nullptr);
    for (size_t i = 0; i < span->n; ++i) {
        auto *m = zmalloc::CacheTestAccess::decode(
            zmalloc::CacheTestAccess::page_map(pc).get(span->page_id + i));
        ASSERT_EQ(m, span);
    }
}

TEST_F(PageCacheTest, ReleaseDoesNotCrashWhenAdjacentMissing) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(5);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    span_owners.release(span);
    SUCCEED();
}

TEST_F(PageCacheTest, BucketListContainsReleasedSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(6);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    const size_t page_id = span->page_id;
    span_owners.release(span);
    auto *mapped = static_cast<zmalloc::Span *>(
        zmalloc::CacheTestAccess::page_map(pc).get(page_id));
    ASSERT_NE(mapped, nullptr);
    EXPECT_FALSE(mapped->is_use);
    EXPECT_TRUE(ContainsSpan(
        zmalloc::CacheTestAccess::span_lists(pc)[mapped->n], mapped));
}

TEST_F(PageCacheTest, NewSpanFromBucketResetsObjectMetadataAndMapsEveryPage) {
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    auto *span = span_owners.allocate(2);
    ASSERT_NE(span, nullptr);
    span->obj_size = 64;
    span->use_count = 1;
    span->free_list =
        reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    span_owners.release(span);
    auto *reused = span_owners.allocate(2);
    ASSERT_NE(reused, nullptr);
    EXPECT_TRUE(reused->is_use);
    EXPECT_EQ(reused->obj_size, 0u);
    EXPECT_EQ(reused->use_count, 0u);
    EXPECT_EQ(reused->free_list, nullptr);
    for (size_t i = 0; i < reused->n; ++i) {
        auto *address = reinterpret_cast<void *>((reused->page_id + i)
                                                 << zmalloc::PAGE_SHIFT);
        EXPECT_EQ(pc.try_map_cached_object_to_span(address), reused);
    }
}

TEST_F(PageCacheTest, MultipleSmallAllocationsDoNotOverlapInMapping) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *a = span_owners.allocate(1);
    zmalloc::Span *b = span_owners.allocate(1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    if (a->page_id != b->page_id) {
        EXPECT_NE(zmalloc::CacheTestAccess::page_map(pc).get(a->page_id),
                  zmalloc::CacheTestAccess::page_map(pc).get(b->page_id));
    }
}

TEST_P(PageCacheSmallSpanParamTest, NewSpanMapsAllPagesAndReleaseKeepsBoundary) {
    NewSpanCheckMappingAndRelease(pc, GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    SpanSizes, PageCacheSmallSpanParamTest,
    ::testing::Values(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u,
                      14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u,
                      25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 48u, 64u, 96u,
                      127u, 128u));

TEST_P(PageCacheLargeSpanParamTest, LargeSpanAllocFree) {
    NewLargeSpanAllocAndFree(pc, GetParam());
}

INSTANTIATE_TEST_SUITE_P(SpanSizes, PageCacheLargeSpanParamTest,
                         ::testing::Values(129u, 256u, 512u));

TEST_F(PageCacheTest, CachedObjectTagIsDecodedAndClearedBeforeReuse) {
    std::lock_guard<std::mutex> lock(pc.page_mtx());
    zmalloc::SpanOwners span_owners(pc);
    zmalloc::Span *span = span_owners.allocate(1);
    span->obj_size = 64;
    void *address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    // 通用查询去除标记，释放后缓存边界映射不得继续携带分配标记。
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), span);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    EXPECT_EQ(pc.map_object_to_span(address), span);
    span_owners.release(span);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);

    span = span_owners.allocate(64);
    address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), span);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    span_owners.release(span);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);

    // 直接系统映射可能被解除，必须保留有锁查询路径。
    span = span_owners.allocate(256);
    address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    span_owners.release(span);
}

TEST_F(PageCacheTest, LargeCacheReusesExactSizeAndClearsAlignedMappings) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    constexpr size_t pages = 256;
    auto *span = span_owners.allocate(pages);
    auto *address = reinterpret_cast<unsigned char *>(span->page_id << zmalloc::PAGE_SHIFT);
    void *extra = address + 65536;
    cache.map_span_page(span, extra);
    span->obj_size = pages * zmalloc::PAGE_SIZE;
    address[0] = 0x5a;
    address[pages * zmalloc::PAGE_SIZE - 1] = 0xa5;
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), nullptr);
    span_owners.release(span);
    EXPECT_EQ(cache.try_map_object_to_span(address), nullptr);
    EXPECT_EQ(cache.try_map_object_to_span(extra), nullptr);
    EXPECT_EQ(cache.statistics().free_bytes, pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.statistics().mapped_bytes, pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), 0u);

    auto *reused = span_owners.allocate(pages);
    EXPECT_EQ(reused, span);
    EXPECT_TRUE(reused->is_use);
    EXPECT_FALSE(reused->is_released);
    EXPECT_EQ(reused->obj_size, 0u);
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), nullptr);
    EXPECT_EQ(cache.try_map_object_to_span(address), reused);
    EXPECT_EQ(cache.try_map_object_to_span(extra), nullptr);
    EXPECT_EQ(address[0], 0);
    EXPECT_EQ(address[pages * zmalloc::PAGE_SIZE - 1], 0);
    span_owners.release(reused);
}

TEST_F(PageCacheTest, LargeCacheBudgetEvictsOldestAndLeavesLiveObjectsIntact) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    constexpr size_t pages = 256;
    constexpr size_t bytes = pages * zmalloc::PAGE_SIZE;
    zmalloc::Span *spans[10];
    void *addresses[10];
    for (size_t i = 0; i < 10; ++i) {
        spans[i] = span_owners.allocate(pages);
        addresses[i] = reinterpret_cast<void *>(spans[i]->page_id << zmalloc::PAGE_SHIFT);
        static_cast<unsigned char *>(addresses[i])[bytes - 1] = 0x5a;
    }
    for (size_t i = 0; i < 9; ++i) {
        span_owners.release(spans[i]);
        EXPECT_LE(zmalloc::CacheTestAccess::large_cached_bytes(cache),
                  zmalloc::LARGE_CACHE_BUDGET);
        EXPECT_EQ(cache.try_map_object_to_span(addresses[i]), nullptr);
    }
    EXPECT_EQ(cache.statistics().free_bytes, zmalloc::LARGE_CACHE_BUDGET);
    EXPECT_EQ(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET + bytes);
    EXPECT_EQ(cache.map_object_to_span(addresses[9]), spans[9]);
    EXPECT_EQ(static_cast<unsigned char *>(addresses[9])[bytes - 1], 0x5a);
    span_owners.release(spans[9]);
    EXPECT_EQ(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET);
    // 精确尺寸，不把大缓存块拆给更小的请求。
    auto *different = span_owners.allocate(129);
    EXPECT_EQ(different->n, 129u);
    EXPECT_EQ(cache.statistics().mapped_bytes,
              zmalloc::LARGE_CACHE_BUDGET + 129 * zmalloc::PAGE_SIZE);
    span_owners.release(different);
    EXPECT_LE(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET);
}

TEST_F(PageCacheTest, BeyondLargeCacheLimitStillUnmapsImmediately) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    auto *span = span_owners.allocate(zmalloc::LARGE_CACHE_MAX_PAGES + 1);
    void *address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    span_owners.release(span);
    EXPECT_EQ(cache.statistics().mapped_bytes, 0u);
    EXPECT_EQ(cache.statistics().free_bytes, 0u);
    EXPECT_EQ(cache.try_map_object_to_span(address), nullptr);
}

TEST_F(PageCacheTest, AutomaticReleaseIsBoundedAndKeepsLivePagesIntact) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    constexpr size_t pages = zmalloc::NPAGES - 1;
    constexpr size_t bytes = pages * zmalloc::PAGE_SIZE;
    zmalloc::Span *spans[65];
    for (auto &span : spans) {
        span = span_owners.allocate(pages);
        auto *p = reinterpret_cast<unsigned char *>(span->page_id << zmalloc::PAGE_SHIFT);
        std::memset(p, 0x5a, bytes);
    }
    for (size_t i = 0; i < 64; ++i) {
        const auto before = cache.statistics();
        errno = EDOM;
        span_owners.release(spans[i]);
        EXPECT_EQ(errno, EDOM);
        const auto after = cache.statistics();
        EXPECT_LE(after.total_released_bytes - before.total_released_bytes,
                  zmalloc::AUTO_RELEASE_MAX_BYTES);
        EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache),
                  after.free_bytes - after.released_bytes);
        if ((i + 1) * bytes < zmalloc::AUTO_RELEASE_MIN_FREE_BYTES) {
            EXPECT_EQ(after.total_released_bytes, 0u);
        }
    }
    const auto stats = cache.statistics();
    if (zmalloc::AUTO_RELEASE_ENABLED) {
        EXPECT_GT(stats.released_bytes, 0u);
        EXPECT_LT(stats.released_bytes, stats.free_bytes);
    } else {
        EXPECT_EQ(stats.released_bytes, 0u);
        EXPECT_EQ(stats.total_released_bytes, 0u);
    }
    auto *live = reinterpret_cast<unsigned char *>(spans[64]->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(live[0], 0x5a);
    EXPECT_EQ(live[bytes - 1], 0x5a);
    EXPECT_EQ(cache.release_free_pages(), stats.free_bytes - stats.released_bytes);
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache), 0u);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    span_owners.release(spans[64]);
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache), bytes);
}

#if ZMALLOC_AUTO_RELEASE_ENABLED
TEST_F(PageCacheTest, AutomaticReleaseCursorSurvivesLargeCacheEviction) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    zmalloc::Span *large[8];
    zmalloc::Span *normal[16];
    for (auto &span : large)
        span = span_owners.allocate(256);
    auto *first = span_owners.allocate(zmalloc::LARGE_CACHE_MAX_PAGES);
    auto *second = span_owners.allocate(zmalloc::LARGE_CACHE_MAX_PAGES);
    for (auto &span : normal)
        span = span_owners.allocate(zmalloc::NPAGES - 1);
    for (auto *span : large)
        span_owners.release(span);
    for (auto *span : normal)
        span_owners.release(span);
    ASSERT_EQ(zmalloc::CacheTestAccess::release_bucket(cache), 0u);
    ASSERT_NE(zmalloc::CacheTestAccess::release_cursor(cache), nullptr);
    ASSERT_NE(zmalloc::CacheTestAccess::release_cursor(cache),
              zmalloc::CacheTestAccess::large_spans(cache).end());
    // 显式回收后保留扫描位置，驱逐覆盖该位置的大块，后续扫描不能访问旧元数据。
    cache.release_free_pages();
    span_owners.release(first);
    span_owners.release(second);
    EXPECT_EQ(zmalloc::CacheTestAccess::release_cursor(cache), first);
    auto *reused = span_owners.allocate(zmalloc::LARGE_CACHE_MAX_PAGES);
    EXPECT_EQ(reused, second);
    span_owners.release(reused);
    for (size_t i = 0; i < 16; ++i) {
        normal[i] = span_owners.allocate(zmalloc::NPAGES - 1);
    }
    for (auto *span : normal)
        span_owners.release(span);
    const auto stats = cache.statistics();
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache),
              stats.free_bytes - stats.released_bytes);
    EXPECT_LE(zmalloc::CacheTestAccess::large_cached_bytes(cache),
              zmalloc::LARGE_CACHE_BUDGET);
    cache.release_free_pages();
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache), 0u);
}

TEST_F(PageCacheTest, AutomaticReleaseCursorSurvivesAllocationSplitAndMerge) {
    auto owned_cache = zmalloc::CacheTestAccess::make_page_cache();
    auto &cache = *owned_cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    zmalloc::SpanOwners span_owners(cache);
    constexpr size_t pages = zmalloc::NPAGES - 1;
    zmalloc::Span *spans[48];
    for (auto &span : spans) {
        span = span_owners.allocate(pages);
    }
    for (size_t i = 0; i < 32; ++i) {
        span_owners.release(spans[i]);
    }
    ASSERT_NE(zmalloc::CacheTestAccess::release_cursor(cache), nullptr);
    ASSERT_NE(zmalloc::CacheTestAccess::release_cursor(cache),
              zmalloc::CacheTestAccess::span_lists(cache)[pages].end());
    // 分配直到命中跨轮保存的游标，再切分剩余桶并将页合并归还。
    zmalloc::Span *held[32];
    size_t count = 0;
    zmalloc::Span *target = zmalloc::CacheTestAccess::release_cursor(cache);
    do {
        held[count++] = span_owners.allocate(pages);
    } while (held[count - 1] != target && count < 32);
    ASSERT_EQ(held[count - 1], target);
    auto *part = span_owners.allocate(1);
    span_owners.release(part);
    for (size_t i = 0; i < count; ++i) {
        span_owners.release(held[i]);
    }
    for (size_t i = 32; i < 48; ++i) {
        span_owners.release(spans[i]);
    }
    const auto stats = cache.statistics();
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache),
              stats.free_bytes - stats.released_bytes);
    EXPECT_GT(stats.total_released_bytes, 0u);
    cache.release_free_pages();
    EXPECT_EQ(zmalloc::CacheTestAccess::unreleased_bytes(cache), 0u);
}

#endif

TEST_F(PageCacheTest, ArbitraryQueriesHoldPageLockDuringSpanReuse) {
    std::atomic<void *> candidate(nullptr);
    std::atomic<bool> reader_ready(false);
    std::atomic<bool> done(false);
    std::atomic<bool> valid(true);
    std::thread reader([&] {
        reader_ready.store(true, std::memory_order_release);
        do {
            // candidate 不保证对象存活，必须从查询开始就持锁，防止 Span 被复用。
            std::lock_guard<std::mutex> lock(pc.page_mtx());
            zmalloc::SpanOwners span_owners(pc);
            void *address = candidate.load(std::memory_order_relaxed);
            zmalloc::Span *span = pc.try_map_object_to_span(address);
            if (span != nullptr) {
                const zmalloc::PageId id =
                    reinterpret_cast<zmalloc::PageId>(address) >>
                    zmalloc::PAGE_SHIFT;
                if (span->n == 0 || id < span->page_id ||
                    id - span->page_id >= span->n ||
                    (!span->is_use && span->obj_size != 0)) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
        } while (!done.load(std::memory_order_acquire));
    });
    std::thread writer([&] {
        while (!reader_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (size_t round = 0; round < 1000; ++round) {
            std::lock_guard<std::mutex> lock(pc.page_mtx());
            zmalloc::SpanOwners span_owners(pc);
            // 同时覆盖缓存块拆分/合并，以及系统映射释放后的元数据复用。
            const size_t pages = round % 2 == 0 ? round % 128 + 1 : 256;
            zmalloc::Span *span = span_owners.allocate(pages);
            span->obj_size = pages * zmalloc::PAGE_SIZE;
            candidate.store(
                reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT),
                std::memory_order_relaxed);
            span_owners.release(span);
        }
        done.store(true, std::memory_order_release);
    });
    writer.join();
    reader.join();
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}
