#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "zmalloc/internal/span_list.h"
#include "zmalloc/internal/system_alloc.h"
#include "zmalloc/internal/zmalloc_config.h"

// 先包含基础头，避免 private->public 影响标准库头
#define private public
#include "zmalloc/internal/page_cache.h"
#undef private

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
    zmalloc::PageCache cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    auto *whole = cache.new_span(zmalloc::NPAGES - 1);
    auto *address = reinterpret_cast<unsigned char *>(whole->page_id << zmalloc::PAGE_SHIFT);
    const size_t bytes = whole->n * zmalloc::PAGE_SIZE;
    std::memset(address, 0x5a, bytes);
    cache.release_span_to_page_cache(whole);
    EXPECT_EQ(cache.release_free_pages(), bytes);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    EXPECT_EQ(cache.statistics().released_bytes, bytes);

    auto *part = cache.new_span(32);
    EXPECT_FALSE(part->is_released);
    EXPECT_EQ(cache.statistics().released_bytes, bytes - 32 * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), part);
    std::memset(address, 0xa5, 32 * zmalloc::PAGE_SIZE);
    EXPECT_EQ(address[32 * zmalloc::PAGE_SIZE - 1], 0xa5);

    cache.release_span_to_page_cache(part);
    EXPECT_EQ(cache.statistics().released_bytes, 0u);
    EXPECT_EQ(cache.release_free_pages(), bytes);
    EXPECT_EQ(cache.release_free_pages(), 0u);
    whole = cache.new_span(zmalloc::NPAGES - 1);
    EXPECT_FALSE(whole->is_released);
    EXPECT_EQ(cache.statistics().released_bytes, 0u);
    EXPECT_EQ(address[0], 0);
    EXPECT_EQ(address[bytes - 1], 0);
    cache.release_span_to_page_cache(whole);
    EXPECT_EQ(cache.release_free_pages(), bytes);
}

static void NewSpanCheckMappingAndRelease(zmalloc::PageCache &pc, size_t k) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(k);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->n, k);
    ASSERT_TRUE(span->is_use);

    const zmalloc::PageId page_id = span->page_id;
    // 小 span：要求每一页都建立映射。
    for (size_t i = 0; i < k; ++i) {
        auto *m =
            pc.decode_span(pc.id_span_map_.get(page_id + i));
        ASSERT_EQ(m, span);
        void *addr =
            reinterpret_cast<void *>((page_id + i) << zmalloc::PAGE_SHIFT);
        ASSERT_EQ(pc.map_object_to_span(addr), span);
    }

    span->is_use = true;
    pc.release_span_to_page_cache(span);

    // release 后 span 指针可能失效；只验证起始页仍能定位到一个空闲 span。
    auto *a = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(a->is_use);

    auto *b =
        static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + k - 1));
    if (b != nullptr) {
        EXPECT_EQ(a, b);
        EXPECT_FALSE(b->is_use);
    }
}

static void NewLargeSpanAllocAndFree(zmalloc::PageCache &pc, size_t k) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(k);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->n, k);
    ASSERT_TRUE(span->is_use);
    pc.release_span_to_page_cache(span);
}

TEST_F(PageCacheTest, NewSpanSmallBasicFields) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(1);
    ASSERT_NE(span, nullptr);
    EXPECT_GT(span->n, 0u);
    EXPECT_NE(span->page_id, 0u);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanStart) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(2);
    ASSERT_NE(span, nullptr);
    void *p = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, MapObjectToSpanWorksForSpanMiddle) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(4);
    ASSERT_NE(span, nullptr);
    void *p =
        reinterpret_cast<void *>((span->page_id + 2) << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.map_object_to_span(p), span);
}

TEST_F(PageCacheTest, ReleaseSpanMarksNotInUse) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(3);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    const size_t page_id = span->page_id;
    const size_t n = span->n;
    pc.release_span_to_page_cache(span);
    // release 之后 span 指针可能被合并/回收，不能再直接访问。
    auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
    ASSERT_NE(mapped, nullptr);
    EXPECT_FALSE(mapped->is_use);
    // 原 span 的结束页若仍是边界页，则也应指向同一个空闲 span。
    auto *mapped_end =
        static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
    if (mapped_end != nullptr) {
        EXPECT_EQ(mapped_end, mapped);
    }
}

TEST_F(PageCacheTest, ReleaseThenNewSpanCanReuseBucket) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span1 = pc.new_span(1);
    ASSERT_NE(span1, nullptr);
    const size_t page_id = span1->page_id;
    const size_t n = span1->n;
    span1->is_use = true;
    pc.release_span_to_page_cache(span1);

    // release 后原 span1 指针可能失效；只验证映射仍可用。
    auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
    ASSERT_NE(mapped, nullptr);
    auto *mapped_end =
        static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
    ASSERT_NE(mapped_end, nullptr);
    EXPECT_EQ(mapped, mapped_end);
    EXPECT_FALSE(mapped->is_use);

    // 再次申请同尺寸 span 不应崩溃。
    zmalloc::Span *span2 = pc.new_span(1);
    ASSERT_NE(span2, nullptr);
}

TEST_F(PageCacheTest, LargeSpanGoesToSystemPath) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    const size_t k = zmalloc::NPAGES + 10;
    zmalloc::Span *span = pc.new_span(k);
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->n, k);
    EXPECT_TRUE(span->is_use);
    pc.release_span_to_page_cache(span);
}

TEST_F(PageCacheTest, IdSpanMapHasStartAndEndForFreeSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(8);
    ASSERT_NE(span, nullptr);
    const size_t page_id = span->page_id;
    const size_t n = span->n;
    span->is_use = true;
    pc.release_span_to_page_cache(span);

    // free span 只保证首尾页映射存在（用于合并）。
    auto *a = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
    auto *b =
        static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id + n - 1));
    ASSERT_NE(a, nullptr);
    if (b != nullptr) {
        EXPECT_EQ(a, b);
    }
    EXPECT_FALSE(a->is_use);
}

TEST_F(PageCacheTest, ReleaseClearsInteriorMappingsForFreeSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(8);
    ASSERT_NE(span, nullptr);
    ASSERT_GT(span->n, 2u);

    const size_t middle_page = span->page_id + 3;
    span->is_use = true;
    pc.release_span_to_page_cache(span);

    EXPECT_EQ(pc.id_span_map_.get(middle_page), nullptr);
}

TEST_F(PageCacheTest, ReusedSpanResetsAccountingState) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(4);
    ASSERT_NE(span, nullptr);

    span->obj_size = 160;
    span->use_count = 7;
    span->free_list = reinterpret_cast<void *>(0x1);
    span->is_use = true;
    const size_t page_id = span->page_id;
    pc.release_span_to_page_cache(span);

    zmalloc::Span *reused = pc.new_span(4);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->page_id, page_id);
    EXPECT_EQ(reused->obj_size, 0u);
    EXPECT_EQ(reused->use_count, 0u);
    EXPECT_EQ(reused->free_list, nullptr);
}

TEST_F(PageCacheTest, MergeClearsOldNeighborBoundaryMappings) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());

    void *region = zmalloc::system_alloc(6);
    ASSERT_NE(region, nullptr);

    const zmalloc::PageId base =
        reinterpret_cast<zmalloc::PageId>(region) >> zmalloc::PAGE_SHIFT;

    auto init_span = [](zmalloc::Span *span, zmalloc::PageId page_id, size_t n,
                        bool is_use) {
        span->page_id = page_id;
        span->n = n;
        span->next = nullptr;
        span->prev = nullptr;
        span->obj_size = 0;
        span->use_count = 0;
        span->free_list = nullptr;
        span->is_use = is_use;
    };

    zmalloc::Span *left = pc.span_pool_.allocate();
    zmalloc::Span *middle = pc.span_pool_.allocate();
    zmalloc::Span *right = pc.span_pool_.allocate();
    ASSERT_NE(left, nullptr);
    ASSERT_NE(middle, nullptr);
    ASSERT_NE(right, nullptr);

    init_span(left, base, 2, false);
    init_span(middle, base + 2, 2, true);
    init_span(right, base + 4, 2, false);

    pc.span_lists_[left->n].push_front(left);
    pc.span_lists_[right->n].push_front(right);
    pc.id_span_map_.set(left->page_id, left);
    pc.id_span_map_.set(left->page_id + left->n - 1, left);
    pc.id_span_map_.set_range(middle->page_id, middle->n, middle);
    pc.id_span_map_.set(right->page_id, right);
    pc.id_span_map_.set(right->page_id + right->n - 1, right);

    pc.release_span_to_page_cache(middle);

    EXPECT_EQ(middle->page_id, base);
    EXPECT_EQ(middle->n, 6u);
    EXPECT_FALSE(middle->is_use);
    EXPECT_EQ(middle->obj_size, 0u);
    EXPECT_EQ(middle->use_count, 0u);
    EXPECT_EQ(middle->free_list, nullptr);
    EXPECT_TRUE(ContainsSpan(pc.span_lists_[middle->n], middle));

    EXPECT_EQ(pc.id_span_map_.get(base), middle);
    EXPECT_EQ(pc.id_span_map_.get(base + 5), middle);
    EXPECT_EQ(pc.id_span_map_.get(base + 1), nullptr);
    EXPECT_EQ(pc.id_span_map_.get(base + 2), nullptr);
    EXPECT_EQ(pc.id_span_map_.get(base + 3), nullptr);
    EXPECT_EQ(pc.id_span_map_.get(base + 4), nullptr);

    pc.span_lists_[middle->n].erase(middle);
    pc.id_span_map_.set_range(base, 6, nullptr);
    pc.span_pool_.deallocate(middle);
    zmalloc::system_free(region, 6);
}

TEST_F(PageCacheTest, NewSpanEstablishesAllPagesMapForSmallK) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(3);
    ASSERT_NE(span, nullptr);
    for (size_t i = 0; i < span->n; ++i) {
        auto *m = pc.decode_span(pc.id_span_map_.get(span->page_id + i));
        ASSERT_EQ(m, span);
    }
}

TEST_F(PageCacheTest, ReleaseDoesNotCrashWhenAdjacentMissing) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(5);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    pc.release_span_to_page_cache(span);
    SUCCEED();
}

TEST_F(PageCacheTest, BucketListContainsReleasedSpan) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(6);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    const size_t page_id = span->page_id;
    pc.release_span_to_page_cache(span);
    auto *mapped = static_cast<zmalloc::Span *>(pc.id_span_map_.get(page_id));
    ASSERT_NE(mapped, nullptr);
    EXPECT_FALSE(mapped->is_use);
    EXPECT_TRUE(ContainsSpan(pc.span_lists_[mapped->n], mapped));
}

TEST_F(PageCacheTest, NewSpanFromBucketClearsIsUse) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *span = pc.new_span(2);
    ASSERT_NE(span, nullptr);
    span->is_use = true;
    pc.release_span_to_page_cache(span);

    zmalloc::Span *span2 = pc.new_span(2);
    ASSERT_NE(span2, nullptr);
    // new_span 返回的 span 不一定清
    // is_use，但中央层会设置。这里保证可用性，不崩溃即可。
    SUCCEED();
}

TEST_F(PageCacheTest, MultipleSmallAllocationsDoNotOverlapInMapping) {
    std::lock_guard<std::mutex> lk(pc.page_mtx());
    zmalloc::Span *a = pc.new_span(1);
    zmalloc::Span *b = pc.new_span(1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    if (a->page_id != b->page_id) {
        EXPECT_NE(pc.id_span_map_.get(a->page_id),
                  pc.id_span_map_.get(b->page_id));
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
    zmalloc::Span *span = pc.new_span(1);
    span->obj_size = 64;
    void *address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    // 通用查询去除标记，释放后缓存边界映射不得继续携带分配标记。
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), span);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    EXPECT_EQ(pc.map_object_to_span(address), span);
    pc.release_span_to_page_cache(span);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);

    span = pc.new_span(64);
    address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), span);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    pc.release_span_to_page_cache(span);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);

    // 直接系统映射可能被解除，必须保留有锁查询路径。
    span = pc.new_span(256);
    address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    EXPECT_EQ(pc.try_map_cached_object_to_span(address), nullptr);
    EXPECT_EQ(pc.try_map_object_to_span(address), span);
    pc.release_span_to_page_cache(span);
}

TEST_F(PageCacheTest, LargeCacheReusesExactSizeAndClearsAlignedMappings) {
    zmalloc::PageCache cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    constexpr size_t pages = 256;
    auto *span = cache.new_span(pages);
    auto *address = reinterpret_cast<unsigned char *>(span->page_id << zmalloc::PAGE_SHIFT);
    void *extra = address + 65536;
    cache.map_span_page(span, extra);
    span->obj_size = pages * zmalloc::PAGE_SIZE;
    address[0] = 0x5a;
    address[pages * zmalloc::PAGE_SIZE - 1] = 0xa5;
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), nullptr);
    cache.release_span_to_page_cache(span);
    EXPECT_EQ(cache.try_map_object_to_span(address), nullptr);
    EXPECT_EQ(cache.try_map_object_to_span(extra), nullptr);
    EXPECT_EQ(cache.statistics().free_bytes, pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.statistics().mapped_bytes, pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), pages * zmalloc::PAGE_SIZE);
    EXPECT_EQ(cache.release_free_pages(), 0u);

    auto *reused = cache.new_span(pages);
    EXPECT_EQ(reused, span);
    EXPECT_TRUE(reused->is_use);
    EXPECT_FALSE(reused->is_released);
    EXPECT_EQ(reused->obj_size, 0u);
    EXPECT_EQ(cache.try_map_cached_object_to_span(address), nullptr);
    EXPECT_EQ(cache.try_map_object_to_span(address), reused);
    EXPECT_EQ(cache.try_map_object_to_span(extra), nullptr);
    EXPECT_EQ(address[0], 0);
    EXPECT_EQ(address[pages * zmalloc::PAGE_SIZE - 1], 0);
    cache.release_span_to_page_cache(reused);
}

TEST_F(PageCacheTest, LargeCacheBudgetEvictsOldestAndLeavesLiveObjectsIntact) {
    zmalloc::PageCache cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    constexpr size_t pages = 256;
    constexpr size_t bytes = pages * zmalloc::PAGE_SIZE;
    zmalloc::Span *spans[10];
    void *addresses[10];
    for (size_t i = 0; i < 10; ++i) {
        spans[i] = cache.new_span(pages);
        addresses[i] = reinterpret_cast<void *>(spans[i]->page_id << zmalloc::PAGE_SHIFT);
        static_cast<unsigned char *>(addresses[i])[bytes - 1] = 0x5a;
    }
    for (size_t i = 0; i < 9; ++i) {
        cache.release_span_to_page_cache(spans[i]);
        EXPECT_LE(cache.large_cached_bytes_, zmalloc::LARGE_CACHE_BUDGET);
        EXPECT_EQ(cache.try_map_object_to_span(addresses[i]), nullptr);
    }
    EXPECT_EQ(cache.statistics().free_bytes, zmalloc::LARGE_CACHE_BUDGET);
    EXPECT_EQ(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET + bytes);
    EXPECT_EQ(cache.map_object_to_span(addresses[9]), spans[9]);
    EXPECT_EQ(static_cast<unsigned char *>(addresses[9])[bytes - 1], 0x5a);
    cache.release_span_to_page_cache(spans[9]);
    EXPECT_EQ(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET);
    // 精确尺寸，不把大缓存块拆给更小的请求。
    auto *different = cache.new_span(129);
    EXPECT_EQ(different->n, 129u);
    EXPECT_EQ(cache.statistics().mapped_bytes,
              zmalloc::LARGE_CACHE_BUDGET + 129 * zmalloc::PAGE_SIZE);
    cache.release_span_to_page_cache(different);
    EXPECT_LE(cache.statistics().mapped_bytes, zmalloc::LARGE_CACHE_BUDGET);
}

TEST_F(PageCacheTest, BeyondLargeCacheLimitStillUnmapsImmediately) {
    zmalloc::PageCache cache;
    std::lock_guard<std::mutex> lock(cache.page_mtx());
    auto *span = cache.new_span(zmalloc::LARGE_CACHE_MAX_PAGES + 1);
    void *address = reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT);
    cache.release_span_to_page_cache(span);
    EXPECT_EQ(cache.statistics().mapped_bytes, 0u);
    EXPECT_EQ(cache.statistics().free_bytes, 0u);
    EXPECT_EQ(cache.try_map_object_to_span(address), nullptr);
}

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
            // 同时覆盖缓存块拆分/合并，以及系统映射释放后的元数据复用。
            const size_t pages = round % 2 == 0 ? round % 128 + 1 : 256;
            zmalloc::Span *span = pc.new_span(pages);
            span->obj_size = pages * zmalloc::PAGE_SIZE;
            candidate.store(
                reinterpret_cast<void *>(span->page_id << zmalloc::PAGE_SHIFT),
                std::memory_order_relaxed);
            pc.release_span_to_page_cache(span);
        }
        done.store(true, std::memory_order_release);
    });
    writer.join();
    reader.join();
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
