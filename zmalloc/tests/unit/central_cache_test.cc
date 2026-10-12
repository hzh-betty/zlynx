#include "../support/cache_test_access.h"
#include <gtest/gtest.h>
#include <unordered_map>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "zmalloc/internal/free_list.h"
#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/size_class.h"
#include "zmalloc/internal/span_list.h"
#include "zmalloc/internal/zmalloc_config.h"

#include "zmalloc/internal/central_cache.h"

namespace {

class CentralObjectOwners {
  public:
    explicit CentralObjectOwners(zmalloc::CentralCache &cache)
        : cache_(cache) {}

    ~CentralObjectOwners() {
        for (auto entry : live_) {
            zmalloc::next_obj(entry.first) = nullptr;
            cache_.release_list_to_spans(entry.first, entry.second);
        }
    }

    size_t fetch_range_obj(void *&start, void *&end, size_t count, size_t size,
                           size_t index) {
        const auto fetched =
            cache_.fetch_range_obj(start, end, count, size, index);
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            for (void *p = start; p; p = zmalloc::next_obj(p))
                live_.emplace(p, index);
        } catch (...) {
            release_list_to_spans(start, index);
            throw;
        }
        return fetched;
    }

    void release_list_to_spans(void *start, size_t index) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (void *p = start; p; p = zmalloc::next_obj(p))
                live_.erase(p);
        }
        cache_.release_list_to_spans(start, index);
    }

  private:
    zmalloc::CentralCache &cache_;
    std::mutex mutex_;
    std::unordered_map<void *, size_t> live_;
};

struct CentralChainDeleter {
    CentralObjectOwners &cache;
    size_t index;

    void operator()(void *start) const {
        cache.release_list_to_spans(start, index);
    }
};

using CentralChain = std::unique_ptr<void, CentralChainDeleter>;

static bool ContainsSpan(zmalloc::SpanList &list, zmalloc::Span *target) {
    for (zmalloc::Span *it = list.begin(); it != list.end(); it = it->next) {
        if (it == target) {
            return true;
        }
    }
    return false;
}

static size_t CountSpans(zmalloc::SpanList &list) {
    size_t n = 0;
    for (zmalloc::Span *it = list.begin(); it != list.end(); it = it->next) {
        ++n;
    }
    return n;
}

static size_t CountChain(void *start) {
    size_t n = 0;
    while (start) {
        start = zmalloc::next_obj(start);
        ++n;
    }
    return n;
}

static void FetchAndRelease(zmalloc::CentralCache &cc, size_t batch_n,
                            size_t size) {
    CentralObjectOwners owners(cc);
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, batch_n, size, zmalloc::SizeClass::index_fast(size));
    ASSERT_GE(got, 1u);
    ASSERT_LE(got, batch_n);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(end, nullptr);
    ASSERT_EQ(CountChain(start), got);
    ASSERT_EQ(zmalloc::next_obj(end), nullptr);
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(size));
}

static void FetchTwiceConcatAndRelease(zmalloc::CentralCache &cc, size_t n1,
                                       size_t n2, size_t size) {
    CentralObjectOwners owners(cc);
    void *s1 = nullptr;
    void *e1 = nullptr;
    const size_t g1 = owners.fetch_range_obj(
        s1, e1, n1, size, zmalloc::SizeClass::index_fast(size));
    ASSERT_GE(g1, 1u);
    ASSERT_EQ(zmalloc::next_obj(e1), nullptr);

    void *s2 = nullptr;
    void *e2 = nullptr;
    const size_t g2 = owners.fetch_range_obj(
        s2, e2, n2, size, zmalloc::SizeClass::index_fast(size));
    ASSERT_GE(g2, 1u);
    ASSERT_EQ(zmalloc::next_obj(e2), nullptr);

    // 拼接链表：s1 -> ... -> e1 -> s2 -> ... -> e2
    zmalloc::next_obj(e1) = s2;
    owners.release_list_to_spans(s1, zmalloc::SizeClass::index_fast(size));
}

static void ReverseChain(void *&start, void *&end) {
    void *prev = nullptr;
    void *cur = start;
    end = start;
    while (cur) {
        void *next = zmalloc::next_obj(cur);
        zmalloc::next_obj(cur) = prev;
        prev = cur;
        cur = next;
    }
    start = prev;
}

} // namespace

class CentralCacheTest : public ::testing::Test {
  protected:
    zmalloc::CentralCache &cc = zmalloc::CentralCache::get_instance();
    zmalloc::PageCache &pc = zmalloc::PageCache::get_instance();
    CentralObjectOwners owners{cc};
};

class CentralCacheFetchReleaseParamTest
    : public CentralCacheTest,
      public ::testing::WithParamInterface<std::tuple<size_t, size_t>> {};

TEST_F(CentralCacheTest, FetchReturnsAtLeastOne) {
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, 1, 64, zmalloc::SizeClass::index_fast(64));
    ASSERT_GE(got, 1u);
    ASSERT_NE(start, nullptr);
    ASSERT_NE(end, nullptr);
    zmalloc::next_obj(end) = nullptr;
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(64));
}

TEST_F(CentralCacheTest, FetchRespectsUpperBoundN) {
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, 8, 64, zmalloc::SizeClass::index_fast(64));
    ASSERT_GE(got, 1u);
    ASSERT_LE(got, 8u);
    zmalloc::next_obj(end) = nullptr;
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(64));
}

TEST_F(CentralCacheTest, FetchProducesValidChain) {
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, 16, 64, zmalloc::SizeClass::index_fast(64));
    ASSERT_EQ(CountChain(start), got);
    ASSERT_EQ(zmalloc::next_obj(end), nullptr);
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(64));
}

TEST_F(CentralCacheTest, SpanObjSizeMatchesRequest) {
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, 4, 128, zmalloc::SizeClass::index_fast(128));
    ASSERT_GE(got, 1u);
    zmalloc::Span *span = pc.map_object_to_span(start);
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(span->obj_size, zmalloc::SizeClass::round_up_fast(128));
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(128));
}

TEST_F(CentralCacheTest, SpanSplitDoesNotCreatePartialTailObject) {
    const size_t request_size = 152;
    const size_t actual_size = zmalloc::SizeClass::round_up_fast(request_size);

    void *start = nullptr;
    void *end = nullptr;
    const size_t got =
        owners.fetch_range_obj(start, end, 128, request_size,
                               zmalloc::SizeClass::index_fast(request_size));
    ASSERT_GE(got, 1u);

    zmalloc::Span *span = pc.map_object_to_span(start);
    ASSERT_NE(span, nullptr);
    const uintptr_t span_begin = static_cast<uintptr_t>(span->page_id)
                                 << zmalloc::PAGE_SHIFT;
    const uintptr_t span_end =
        span_begin + (static_cast<uintptr_t>(span->n) << zmalloc::PAGE_SHIFT);

    void *cur = start;
    while (cur != nullptr) {
        const uintptr_t obj = reinterpret_cast<uintptr_t>(cur);
        EXPECT_GE(obj, span_begin);
        EXPECT_LE(obj + actual_size, span_end);
        cur = zmalloc::next_obj(cur);
    }

    owners.release_list_to_spans(start,
                                 zmalloc::SizeClass::index_fast(request_size));
}

TEST_F(CentralCacheTest, EmptyNonEmptyListsInitiallySmall) {
    const size_t index = zmalloc::SizeClass::index_fast(64);

    // 刚开始不要求为空，但至少结构可访问。
    EXPECT_LE(
        CountSpans(zmalloc::CacheTestAccess::buckets(cc)[index].nonempty) +
            CountSpans(zmalloc::CacheTestAccess::buckets(cc)[index].empty),
        1024u);
}

TEST_F(CentralCacheTest, DrainSpanMovesToEmptyList) {
    const size_t size = 64;
    const size_t index = zmalloc::SizeClass::index_fast(size);
    ASSERT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].empty.empty());
    ASSERT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.empty());

    void *start = nullptr;
    void *end = nullptr;
    const size_t first = owners.fetch_range_obj(start, end, 1, size, index);
    CentralChain first_chain(start, CentralChainDeleter{owners, index});
    ASSERT_EQ(first, 1u);
    ASSERT_NE(start, nullptr);
    zmalloc::Span *span = pc.map_object_to_span(start);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->use_count, 1u);

    // 已确认独占新 Span；一次性取尽其剩余容量。
    const size_t capacity = (span->n << zmalloc::PAGE_SHIFT) / span->obj_size;
    ASSERT_GT(capacity, 1u);
    const size_t remaining = capacity - 1;

    void *all_start = nullptr;
    void *all_end = nullptr;
    const size_t got =
        owners.fetch_range_obj(all_start, all_end, remaining, size, index);
    CentralChain remaining_chain(all_start, CentralChainDeleter{owners, index});
    ASSERT_EQ(got, remaining);
    ASSERT_EQ(CountChain(all_start), remaining);
    ASSERT_EQ(span->free_list, nullptr);

    // span 应当被放入 empty
    EXPECT_TRUE(
        ContainsSpan(zmalloc::CacheTestAccess::buckets(cc)[index].empty, span));
    EXPECT_FALSE(ContainsSpan(
        zmalloc::CacheTestAccess::buckets(cc)[index].nonempty, span));

    // 归还一个对象（第一次 fetch 的对象）：empty -> nonempty
    first_chain.reset();
    EXPECT_TRUE(ContainsSpan(
        zmalloc::CacheTestAccess::buckets(cc)[index].nonempty, span));
    EXPECT_FALSE(
        ContainsSpan(zmalloc::CacheTestAccess::buckets(cc)[index].empty, span));
    EXPECT_EQ(span->use_count, remaining);
    EXPECT_EQ(CountChain(span->free_list), 1u);

    remaining_chain.reset();
    EXPECT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].empty.empty());
    EXPECT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.empty());
}

TEST_F(CentralCacheTest, ReleaseBatchingDoesNotLoseObjects) {
    const size_t size = 64;
    void *start = nullptr;
    void *end = nullptr;
    const size_t got = owners.fetch_range_obj(
        start, end, 64, size, zmalloc::SizeClass::index_fast(size));
    ASSERT_GE(got, 1u);
    ASSERT_EQ(CountChain(start), got);
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(size));
    SUCCEED();
}

TEST_F(CentralCacheTest, MultipleSizeClassesIndependent) {
    void *s1 = nullptr;
    void *e1 = nullptr;
    void *s2 = nullptr;
    void *e2 = nullptr;
    owners.fetch_range_obj(s1, e1, 8, 64, zmalloc::SizeClass::index_fast(64));
    owners.fetch_range_obj(s2, e2, 8, 128, zmalloc::SizeClass::index_fast(128));
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    zmalloc::Span *a = pc.map_object_to_span(s1);
    zmalloc::Span *b = pc.map_object_to_span(s2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->obj_size, b->obj_size);
    owners.release_list_to_spans(s1, zmalloc::SizeClass::index_fast(64));
    owners.release_list_to_spans(s2, zmalloc::SizeClass::index_fast(128));
}

TEST_F(CentralCacheTest, FetchAfterReleaseStillWorks) {
    void *s = nullptr;
    void *e = nullptr;
    owners.fetch_range_obj(s, e, 16, 64, zmalloc::SizeClass::index_fast(64));
    owners.release_list_to_spans(s, zmalloc::SizeClass::index_fast(64));

    void *s2 = nullptr;
    void *e2 = nullptr;
    const size_t got2 = owners.fetch_range_obj(
        s2, e2, 16, 64, zmalloc::SizeClass::index_fast(64));
    ASSERT_GE(got2, 1u);
    owners.release_list_to_spans(s2, zmalloc::SizeClass::index_fast(64));
}

TEST_F(CentralCacheTest, ReleaseNullIsNoop) {
    owners.release_list_to_spans(nullptr, zmalloc::SizeClass::index_fast(64));
    SUCCEED();
}

TEST_F(CentralCacheTest, FetchDifferentThreadsDoesNotCrash) {
    // 单测里不做重性能压力，只验证跨线程调用可用。
    std::vector<void *> ptrs;

    std::thread t([&] {
        void *s = nullptr;
        void *e = nullptr;
        const size_t got = owners.fetch_range_obj(
            s, e, 32, 64, zmalloc::SizeClass::index_fast(64));
        ptrs.reserve(got);
        void *cur = s;
        while (cur) {
            ptrs.push_back(cur);
            cur = zmalloc::next_obj(cur);
        }
    });
    t.join();

    // 归还（单链表还原）
    for (size_t i = 0; i + 1 < ptrs.size(); ++i) {
        zmalloc::next_obj(ptrs[i]) = ptrs[i + 1];
    }
    if (!ptrs.empty()) {
        zmalloc::next_obj(ptrs.back()) = nullptr;
        owners.release_list_to_spans(ptrs.front(),
                                     zmalloc::SizeClass::index_fast(64));
    }
    SUCCEED();
}

TEST_F(CentralCacheTest, NonEmptyListHeadHasFreeObjectsWhenPresent) {
    const size_t size = 64;
    const size_t index = zmalloc::SizeClass::index_fast(size);

    void *s = nullptr;
    void *e = nullptr;
    owners.fetch_range_obj(s, e, 1, size, zmalloc::SizeClass::index_fast(size));

    // 若 nonempty 非空，则其 head 必须有 free_list。
    zmalloc::Span *front =
        zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.begin();
    if (front != zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.end()) {
        EXPECT_NE(front->free_list, nullptr);
    }

    owners.release_list_to_spans(s, zmalloc::SizeClass::index_fast(size));
}

TEST_F(CentralCacheTest, EmptyListSpansHaveNoFreeObjectsWhenPresent) {
    const size_t size = 64;
    const size_t index = zmalloc::SizeClass::index_fast(size);
    ASSERT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].empty.empty());
    ASSERT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.empty());

    void *s = nullptr;
    void *e = nullptr;
    const size_t first = owners.fetch_range_obj(s, e, 1, size, index);
    CentralChain first_chain(s, CentralChainDeleter{owners, index});
    ASSERT_EQ(first, 1u);
    ASSERT_NE(s, nullptr);
    zmalloc::Span *span = pc.map_object_to_span(s);
    ASSERT_NE(span, nullptr);
    ASSERT_EQ(span->use_count, 1u);
    const size_t capacity = (span->n << zmalloc::PAGE_SHIFT) / span->obj_size;
    ASSERT_GT(capacity, 1u);
    void *all_s = nullptr;
    void *all_e = nullptr;
    const size_t got =
        owners.fetch_range_obj(all_s, all_e, capacity - 1, size, index);
    CentralChain remaining_chain(all_s, CentralChainDeleter{owners, index});
    ASSERT_EQ(got, capacity - 1);
    ASSERT_EQ(CountChain(all_s), got);
    ASSERT_TRUE(
        ContainsSpan(zmalloc::CacheTestAccess::buckets(cc)[index].empty, span));

    // 明确已制造 empty Span，避免空链表上的断言无实际覆盖。
    for (zmalloc::Span *it =
             zmalloc::CacheTestAccess::buckets(cc)[index].empty.begin();
         it != zmalloc::CacheTestAccess::buckets(cc)[index].empty.end();
         it = it->next) {
        EXPECT_EQ(it->free_list, nullptr);
        EXPECT_EQ(it->use_count, (it->n << zmalloc::PAGE_SHIFT) / it->obj_size);
    }

    remaining_chain.reset();
    first_chain.reset();
    EXPECT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].empty.empty());
    EXPECT_TRUE(zmalloc::CacheTestAccess::buckets(cc)[index].nonempty.empty());
}

TEST_P(CentralCacheFetchReleaseParamTest, FetchAndReleaseSizeBatchPair) {
    const size_t size = std::get<0>(GetParam());
    const size_t batch = std::get<1>(GetParam());
    FetchAndRelease(cc, batch, size);
}

INSTANTIATE_TEST_SUITE_P(
    SizeBatchPairs, CentralCacheFetchReleaseParamTest,
    ::testing::Values(std::make_tuple(8u, 1u), std::make_tuple(8u, 8u),
                      std::make_tuple(8u, 64u), std::make_tuple(16u, 1u),
                      std::make_tuple(16u, 32u), std::make_tuple(32u, 1u),
                      std::make_tuple(32u, 32u), std::make_tuple(64u, 1u),
                      std::make_tuple(64u, 2u), std::make_tuple(64u, 8u),
                      std::make_tuple(64u, 32u), std::make_tuple(64u, 64u),
                      std::make_tuple(96u, 8u), std::make_tuple(128u, 1u),
                      std::make_tuple(128u, 16u), std::make_tuple(256u, 1u),
                      std::make_tuple(256u, 8u), std::make_tuple(512u, 1u),
                      std::make_tuple(512u, 8u), std::make_tuple(1024u, 1u),
                      std::make_tuple(1024u, 4u), std::make_tuple(2048u, 1u),
                      std::make_tuple(2048u, 2u), std::make_tuple(4096u, 1u),
                      std::make_tuple(4096u, 2u), std::make_tuple(8192u, 1u),
                      std::make_tuple(8192u, 2u), std::make_tuple(65536u, 1u),
                      std::make_tuple(65536u, 2u), std::make_tuple(131072u, 1u),
                      std::make_tuple(131072u, 2u),
                      std::make_tuple(262144u, 1u)));

TEST_F(CentralCacheTest, FetchTwiceConcatThenRelease) {
    FetchTwiceConcatAndRelease(cc, 8, 8, 64);
}

TEST_F(CentralCacheTest, FetchTwiceConcatDifferentBatchThenRelease) {
    FetchTwiceConcatAndRelease(cc, 1, 32, 128);
}

TEST_F(CentralCacheTest, ReleaseReversedChainDoesNotCrash) {
    void *s = nullptr;
    void *e = nullptr;
    const size_t got = owners.fetch_range_obj(
        s, e, 32, 64, zmalloc::SizeClass::index_fast(64));
    ASSERT_GE(got, 1u);
    ASSERT_EQ(zmalloc::next_obj(e), nullptr);
    void *rev_end = nullptr;
    ReverseChain(s, rev_end);

    // 反转后仍应是合法单链表
    ASSERT_EQ(CountChain(s), got);
    owners.release_list_to_spans(s, zmalloc::SizeClass::index_fast(64));
}

TEST_F(CentralCacheTest, ReleaseInterleavedFetchChainsPreservesAllObjects) {
    const size_t index = zmalloc::SizeClass::index_fast(64);
    void *s1 = nullptr, *e1 = nullptr, *s2 = nullptr, *e2 = nullptr;
    const size_t g1 = owners.fetch_range_obj(s1, e1, 8, 64, index);
    CentralChain first(s1, CentralChainDeleter{owners, index});
    ASSERT_EQ(g1, 8u);
    const size_t g2 = owners.fetch_range_obj(s2, e2, 8, 64, index);
    CentralChain second(s2, CentralChainDeleter{owners, index});
    ASSERT_EQ(g2, 8u);
    void *head = nullptr, *tail = nullptr;
    void *a = first.release(), *b = second.release();
    bool take_first = true;
    while (a || b) {
        void *&source = (take_first && a) || !b ? a : b;
        void *node = source;
        source = zmalloc::next_obj(source);
        if (tail)
            zmalloc::next_obj(tail) = node;
        else
            head = node;
        tail = node;
        take_first = !take_first;
    }
    zmalloc::next_obj(tail) = nullptr;
    CentralChain interleaved(head, CentralChainDeleter{owners, index});
    ASSERT_EQ(CountChain(head), g1 + g2);
    EXPECT_EQ(head, s1);
    EXPECT_EQ(zmalloc::next_obj(head), s2);
    interleaved.reset();
    void *start = nullptr, *end = nullptr;
    const size_t fetched =
        owners.fetch_range_obj(start, end, g1 + g2, 64, index);
    CentralChain again(start, CentralChainDeleter{owners, index});
    ASSERT_EQ(fetched, g1 + g2);
    EXPECT_EQ(CountChain(start), fetched);
}

TEST_F(CentralCacheTest, DifferentSizeClassesInterleavedDoesNotCrash) {
    void *a1 = nullptr;
    void *a2 = nullptr;
    owners.fetch_range_obj(a1, a2, 16, 64, zmalloc::SizeClass::index_fast(64));
    void *b1 = nullptr;
    void *b2 = nullptr;
    owners.fetch_range_obj(b1, b2, 16, 128,
                           zmalloc::SizeClass::index_fast(128));
    owners.release_list_to_spans(a1, zmalloc::SizeClass::index_fast(64));
    owners.release_list_to_spans(b1, zmalloc::SizeClass::index_fast(128));
}

TEST_F(CentralCacheTest, FetchCombinesAvailableObjectsFromDifferentSpans) {
    const size_t size = 4096;
    const size_t capacity =
        zmalloc::SizeClass::num_move_page(size) * zmalloc::PAGE_SIZE / size;
    void *heads[2];
    void *tails[2];
    void *rest[2];
    for (size_t i = 0; i < 2; ++i) {
        ASSERT_EQ(owners.fetch_range_obj(heads[i], tails[i], capacity, size,
                                         zmalloc::SizeClass::index_fast(size)),
                  capacity);
        rest[i] = zmalloc::next_obj(heads[i]);
    }
    ASSERT_NE(pc.map_object_to_span(heads[0]), pc.map_object_to_span(heads[1]));
    for (void *head : heads) {
        zmalloc::next_obj(head) = nullptr;
        owners.release_list_to_spans(head,
                                     zmalloc::SizeClass::index_fast(size));
    }
    void *start = nullptr;
    void *end = nullptr;
    ASSERT_EQ(owners.fetch_range_obj(start, end, 2, size,
                                     zmalloc::SizeClass::index_fast(size)),
              2u);
    EXPECT_NE(pc.map_object_to_span(start), pc.map_object_to_span(end));
    owners.release_list_to_spans(start, zmalloc::SizeClass::index_fast(size));
    for (void *head : rest) {
        owners.release_list_to_spans(head,
                                     zmalloc::SizeClass::index_fast(size));
    }
}

TEST_F(CentralCacheTest, ConcurrentRefillAndLargeReleasePreserveUniqueObjects) {
    constexpr size_t threads = 8;
    constexpr size_t count = 512;
    std::vector<void *> objects[threads];
    std::vector<std::thread> workers;
    for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            while (objects[t].size() < count) {
                void *head = nullptr;
                void *tail = nullptr;
                owners.fetch_range_obj(head, tail, count - objects[t].size(),
                                       4096,
                                       zmalloc::SizeClass::index_fast(4096));
                for (void *p = head; p != nullptr; p = zmalloc::next_obj(p)) {
                    objects[t].push_back(p);
                }
            }
        });
    }
    for (auto &worker : workers)
        worker.join();
    std::unordered_set<void *> unique;
    for (const auto &batch : objects) {
        for (void *p : batch)
            EXPECT_TRUE(unique.insert(p).second);
    }
    workers.clear();
    for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (size_t i = 1; i < count; ++i) {
                zmalloc::next_obj(objects[t][i - 1]) = objects[t][i];
            }
            zmalloc::next_obj(objects[t].back()) = nullptr;
            owners.release_list_to_spans(objects[t].front(),
                                         zmalloc::SizeClass::index_fast(4096));
        });
    }
    for (auto &worker : workers)
        worker.join();
}
