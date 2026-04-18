#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "zmalloc/internal/page_cache.h"
#include "zmalloc/internal/zmalloc_config.h"
#include "zmalloc/zmalloc.h"

// 白盒引入实现文件，覆盖 bootstrap/internal helper 与 wrapper 分支。
#include "../../src/override.cc"

namespace zmalloc {
namespace internal {

TEST(OverrideWhiteboxTest, BootstrapAllocateFreeRoundTrip) {
    void *p = bootstrap_allocate(32);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_bootstrap_pointer(p));
    EXPECT_EQ(bootstrap_size(p), 32u);
    EXPECT_TRUE(
        bootstrap_contains_address(static_cast<void *>(static_cast<char *>(p))));
    bootstrap_free(p);
    EXPECT_FALSE(is_bootstrap_pointer(p));
}

TEST(OverrideWhiteboxTest, BootstrapReallocatePaths) {
    unsigned char *p = static_cast<unsigned char *>(bootstrap_reallocate(nullptr, 8));
    ASSERT_NE(p, nullptr);
    for (size_t i = 0; i < 8; ++i) {
        p[i] = static_cast<unsigned char>(i + 7);
    }

    unsigned char *grown =
        static_cast<unsigned char *>(bootstrap_reallocate(p, 64));
    ASSERT_NE(grown, nullptr);
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(grown[i], static_cast<unsigned char>(i + 7));
    }

    EXPECT_EQ(bootstrap_reallocate(grown, 0), nullptr);
}

TEST(OverrideWhiteboxTest, BootstrapNullAndUnknownPointerPaths) {
    EXPECT_EQ(bootstrap_allocate(0), nullptr);
    EXPECT_EQ(bootstrap_size(nullptr), 0u);
    EXPECT_FALSE(is_bootstrap_pointer(nullptr));
    EXPECT_FALSE(bootstrap_contains_address(nullptr));

    bootstrap_free(nullptr);
    int stack_value = 0;
    bootstrap_free(&stack_value);
    EXPECT_EQ(bootstrap_size(&stack_value), 0u);
}

TEST(OverrideWhiteboxTest, BootstrapContainsAddressTraversesList) {
    void *p1 = bootstrap_allocate(32);
    void *p2 = bootstrap_allocate(32);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    void *inside_old_node = static_cast<void *>(static_cast<char *>(p1) + 8);
    EXPECT_TRUE(bootstrap_contains_address(inside_old_node));
    int stack_value = 0;
    EXPECT_FALSE(bootstrap_contains_address(&stack_value));

    bootstrap_free(p1);
    bootstrap_free(p2);
}

TEST(OverrideWhiteboxTest, AllocateBytesAndAlignedValidation) {
    EXPECT_EQ(allocate_bytes(0), nullptr);

    EXPECT_EQ(aligned_allocate_bytes(64, 24), nullptr); // 非 2 的幂

    const size_t huge_size = std::numeric_limits<size_t>::max() - 64;
    EXPECT_EQ(aligned_allocate_bytes(huge_size, 64), nullptr); // 防溢出分支

    void *small_align = aligned_allocate_bytes(0, alignof(std::max_align_t));
    ASSERT_NE(small_align, nullptr);
    deallocate_bytes(small_align);
}

TEST(OverrideWhiteboxTest, AllocateBytesBootstrapPathWhenInitializing) {
    allocator_ready().store(false, std::memory_order_release);
    tls_initializing_allocator = true;
    void *p = allocate_bytes(24);
    tls_initializing_allocator = false;

    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_bootstrap_pointer(p));
    deallocate_bytes(p);
}

TEST(OverrideWhiteboxTest, ManagedSpanAndManagedSizeChecks) {
    void *p = zmalloc(64);
    ASSERT_NE(p, nullptr);
    Span *span = managed_span(p);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(managed_size(p), 64u);

    void *inside = static_cast<void *>(static_cast<char *>(p) + 1);
    EXPECT_EQ(managed_span(inside), nullptr);

    zfree(p);
}

TEST(OverrideWhiteboxTest, ManagedSpanLargeAndNotInUsePaths) {
    void *large = zmalloc(MAX_BYTES + 4096);
    ASSERT_NE(large, nullptr);

    Span *large_span = managed_span(large);
    ASSERT_NE(large_span, nullptr);
    EXPECT_EQ(managed_span(static_cast<void *>(static_cast<char *>(large) + 1)),
              nullptr);

    void *small = zmalloc(64);
    ASSERT_NE(small, nullptr);
    Span *small_span = PageCache::get_instance().try_map_object_to_span(small);
    ASSERT_NE(small_span, nullptr);
    small_span->is_use = false;
    EXPECT_EQ(managed_span(small), nullptr);
    small_span->is_use = true;

    zfree(small);
    zfree(large);
}

TEST(OverrideWhiteboxTest, UnwrapAlignedPointerSuccessAndFailure) {
    void *aligned = aligned_allocate_bytes(80, 128);
    ASSERT_NE(aligned, nullptr);

    void *raw = nullptr;
    size_t size = 0;
    ASSERT_TRUE(unwrap_aligned_pointer(aligned, &raw, &size));
    EXPECT_NE(raw, nullptr);
    EXPECT_EQ(size, 80u);

    int stack_value = 0;
    EXPECT_FALSE(unwrap_aligned_pointer(&stack_value, nullptr, nullptr));

    deallocate_bytes(aligned);
}

TEST(OverrideWhiteboxTest, UnwrapAlignedPointerEdgeFailures) {
    EXPECT_FALSE(unwrap_aligned_pointer(nullptr, nullptr, nullptr));
    EXPECT_FALSE(unwrap_aligned_pointer(reinterpret_cast<void *>(8), nullptr,
                                        nullptr));

    void *raw = allocate_bytes(128);
    ASSERT_NE(raw, nullptr);
    uintptr_t header_addr =
        reinterpret_cast<uintptr_t>(raw) + sizeof(AlignedHeader);
    auto *header = reinterpret_cast<AlignedHeader *>(header_addr);
    header->magic = 0;
    header->raw = raw;
    header->user_size = 16;
    EXPECT_FALSE(unwrap_aligned_pointer(reinterpret_cast<void *>(
                                            header_addr + sizeof(AlignedHeader)),
                                        nullptr, nullptr));

    header->magic = kAlignedAllocMagic;
    int stack_value = 0;
    header->raw = &stack_value;
    EXPECT_FALSE(unwrap_aligned_pointer(reinterpret_cast<void *>(
                                            header_addr + sizeof(AlignedHeader)),
                                        nullptr, nullptr));
    deallocate_bytes(raw);
}

TEST(OverrideWhiteboxTest, ReallocateBytesCoversAlignedPointerBranch) {
    unsigned char *aligned =
        static_cast<unsigned char *>(aligned_allocate_bytes(32, 128));
    ASSERT_NE(aligned, nullptr);
    for (size_t i = 0; i < 32; ++i) {
        aligned[i] = static_cast<unsigned char>(i ^ 0x5A);
    }

    unsigned char *next = static_cast<unsigned char *>(reallocate_bytes(aligned, 96));
    ASSERT_NE(next, nullptr);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(next[i], static_cast<unsigned char>(i ^ 0x5A));
    }
    deallocate_bytes(next);
}

TEST(OverrideWhiteboxTest, ReallocateBytesNullAndBootstrapPaths) {
    void *p = reallocate_bytes(nullptr, 48);
    ASSERT_NE(p, nullptr);
    deallocate_bytes(p);

    void *boot = bootstrap_allocate(16);
    ASSERT_NE(boot, nullptr);
    void *resized = reallocate_bytes(boot, 64);
    ASSERT_NE(resized, nullptr);
    EXPECT_TRUE(is_bootstrap_pointer(resized));
    deallocate_bytes(resized);
}

TEST(OverrideWhiteboxTest, ShouldUseBootstrapAllocatorStateSwitches) {
    allocator_ready().store(false, std::memory_order_release);
    EXPECT_TRUE(should_use_bootstrap_allocator());

    allocator_ready().store(true, std::memory_order_release);
    tls_allocator_call_depth = 1;
    EXPECT_TRUE(should_use_bootstrap_allocator());
    tls_allocator_call_depth = 0;

    tls_initializing_allocator = true;
    EXPECT_TRUE(should_use_bootstrap_allocator());
    tls_initializing_allocator = false;

    allocator_ready().store(false, std::memory_order_release);
    ensure_allocator_ready();
    EXPECT_TRUE(allocator_ready().load(std::memory_order_acquire));
    ensure_allocator_ready();
}

TEST(OverrideWhiteboxTest, WrapperFunctionsCfreeMemalignPvallocAndDeleteVariants) {
    void *p1 = memalign(64, 33);
    ASSERT_NE(p1, nullptr);
    cfree(p1);

    void *p2 = pvalloc(1);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % PAGE_SIZE, 0u);
    free(p2);

    volatile size_t invalid_alignment = 24;
    EXPECT_EQ(aligned_alloc(static_cast<size_t>(invalid_alignment), 64),
              nullptr); // 非法 alignment
    EXPECT_EQ(aligned_alloc(64, 96), nullptr); // size 不是 alignment 倍数
    void *p3 = aligned_alloc(64, 128);
    ASSERT_NE(p3, nullptr);
    free(p3);

    void *calloc_ptr = calloc(4, 16);
    ASSERT_NE(calloc_ptr, nullptr);
    const unsigned char *bytes = static_cast<const unsigned char *>(calloc_ptr);
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0u);
    }
    free(calloc_ptr);
    EXPECT_EQ(calloc(std::numeric_limits<size_t>::max(), 2), nullptr);

    void *pm = reinterpret_cast<void *>(0x1);
    EXPECT_EQ(posix_memalign(&pm, 4, 32), EINVAL);
    EXPECT_EQ(pm, nullptr);
    volatile std::uintptr_t null_addr = 0;
    void **null_memptr = reinterpret_cast<void **>(
        static_cast<std::uintptr_t>(null_addr));
    EXPECT_EQ(posix_memalign(null_memptr, 16, 32), EINVAL);

    EXPECT_EQ(pvalloc(std::numeric_limits<size_t>::max()), nullptr);

    void *d1 = operator new(24);
    ASSERT_NE(d1, nullptr);
    operator delete[](d1, static_cast<size_t>(24));

    void *d2 = operator new(40);
    ASSERT_NE(d2, nullptr);
    operator delete(d2, std::nothrow);

    void *d3 = operator new(56);
    ASSERT_NE(d3, nullptr);
    operator delete[](d3, std::nothrow);
}

TEST(OverrideWhiteboxTest, IsPowerOfTwoChecks) {
    EXPECT_FALSE(is_power_of_two(0));
    EXPECT_TRUE(is_power_of_two(1));
    EXPECT_TRUE(is_power_of_two(64));
    EXPECT_FALSE(is_power_of_two(96));
}

} // namespace internal
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
