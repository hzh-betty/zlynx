#pragma once
#include "zmalloc/internal/central_cache.h"
#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/thread_cache.h"
#include <array>
#include <memory>
#include <stdexcept>

namespace zmalloc {
// Only internal cache invariants and fault setup need privileged access.
// Callers retain the cache's normal locking and object ownership rules.
struct CacheTestAccess {
    static auto &buckets(CentralCache &c) { return c.free_lists_; }

    static auto &lists(ThreadCache &c) { return c.free_lists_; }

    static void scavenge(ThreadCache &c) { c.scavenge(); }

    static void release_batch(ThreadCache &c, size_t index, size_t count) {
        c.release_batch(index, count);
    }

    static std::unique_ptr<PageCache> make_page_cache() {
        return std::unique_ptr<PageCache>(new PageCache);
    }

    static auto &page_map(PageCache &c) { return c.id_span_map_; }

    static Span *decode(void *entry) { return PageCache::decode_span(entry); }

    static auto &span_lists(PageCache &c) { return c.span_lists_; }

    static auto &large_spans(PageCache &c) { return c.large_spans_; }

    static size_t &unreleased_bytes(PageCache &c) {
        return c.unreleased_free_bytes_;
    }

    static size_t large_cached_bytes(const PageCache &c) {
        return c.large_cached_bytes_;
    }

    static size_t release_bucket(const PageCache &c) {
        return c.auto_release_bucket_;
    }

    static Span *release_cursor(const PageCache &c) {
        return c.auto_release_cursor_;
    }
};
} // namespace zmalloc

namespace zmalloc {
class SpanOwners {
  public:
    explicit SpanOwners(PageCache &cache) : cache_(cache) {}

    ~SpanOwners() {
        for (auto *span : live_)
            if (span)
                cache_.release_span_to_page_cache(span);
    }

    Span *allocate(size_t pages) {
        for (auto &slot : live_)
            if (!slot) {
                slot = cache_.new_span(pages);
                return slot;
            }
        throw std::runtime_error("test Span owner capacity exceeded");
    }

    void release(Span *span) {
        for (auto &slot : live_)
            if (slot == span) {
                slot = nullptr;
                break;
            }
        cache_.release_span_to_page_cache(span);
    }

  private:
    PageCache &cache_;
    std::array<Span *, 256> live_{};
};
} // namespace zmalloc
