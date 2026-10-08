#include "zmalloc/internal/page_cache.h"
#include "zmalloc/zmalloc.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <new>
#include <limits>
#include <malloc.h>
#include <thread>
#include <pthread.h>

#include "zmalloc/internal/thread_cache.h"

#include <gtest/gtest.h>

namespace zmalloc {
namespace {


class AllocatorOverrideTest : public ::testing::Test {};

TEST_F(AllocatorOverrideTest, MallocWorksAfterThreadCacheShutdown) {
    bool finished = false;
    std::thread worker([&] {
        void *live = std::malloc(65);
        get_thread_cache()->shutdown();
        std::free(live);
        void *p = std::malloc(128);
        if (p != nullptr) {
            std::memset(p, 42, 128);
            std::free(p);
            finished = get_thread_cache()->cached_bytes() == 0;
        }
    });
    worker.join();
    EXPECT_TRUE(finished);
}

TEST_F(AllocatorOverrideTest, PthreadDestructorRunsAfterCacheCleanup) {
    struct ExitState {
        bool finished = false;
        void *live = nullptr;
    } state;
    pthread_key_t key;
    ASSERT_EQ(pthread_key_create(&key, [](void *value) {
        auto *state = static_cast<ExitState *>(value);
        std::free(state->live);
        void *p = std::malloc(128);
        if (p != nullptr) {
            std::memset(p, 42, 128);
            std::free(p);
            state->finished = get_thread_cache()->cached_bytes() == 0;
        }
    }), 0);
    int registered = -1;
    std::thread worker([&] {
        state.live = std::malloc(65);
        registered = pthread_setspecific(key, &state);
        if (registered != 0) {
            std::free(state.live);
        }
    });
    worker.join();
    EXPECT_EQ(registered, 0);
    EXPECT_TRUE(state.finished);
    EXPECT_EQ(pthread_key_delete(key), 0);
}

#if defined(__GLIBC__)
extern "C" void *__libc_malloc(size_t size) noexcept;
#endif

TEST_F(AllocatorOverrideTest, MallocPointersAreTrackedByPageCache) {
    void *ptr = std::malloc(64);
    ASSERT_NE(ptr, nullptr);

    Span *span = PageCache::get_instance().map_object_to_span(ptr);
    EXPECT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 64u);

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, MallocMeetsMaxAlignTAlignment) {
    void *ptr = std::malloc(1);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignof(std::max_align_t), 0u);

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, CallocZeroInitializesMemory) {
    auto *ptr =
        static_cast<unsigned char *>(std::calloc(32, sizeof(unsigned char)));
    ASSERT_NE(ptr, nullptr);

    Span *span = PageCache::get_instance().map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 32u);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(ptr[i], 0u);
    }

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, ReallocPreservesPrefixWhenGrowing) {
    auto *ptr = static_cast<unsigned char *>(std::malloc(32));
    ASSERT_NE(ptr, nullptr);
    for (size_t i = 0; i < 32; ++i) {
        ptr[i] = static_cast<unsigned char>(i);
    }

    auto *grown = static_cast<unsigned char *>(std::realloc(ptr, 128));
    ASSERT_NE(grown, nullptr);
    Span *span = PageCache::get_instance().map_object_to_span(grown);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 128u);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(grown[i], static_cast<unsigned char>(i));
    }

    std::free(grown);
}

TEST_F(AllocatorOverrideTest, ReallocPreservesPrefixWhenShrinking) {
    auto *ptr = static_cast<unsigned char *>(std::malloc(128));
    ASSERT_NE(ptr, nullptr);
    for (size_t i = 0; i < 128; ++i) {
        ptr[i] = static_cast<unsigned char>(255 - i);
    }

    auto *shrunk = static_cast<unsigned char *>(std::realloc(ptr, 24));
    ASSERT_NE(shrunk, nullptr);
    Span *span = PageCache::get_instance().map_object_to_span(shrunk);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 24u);
    for (size_t i = 0; i < 24; ++i) {
        EXPECT_EQ(shrunk[i], static_cast<unsigned char>(255 - i));
    }

    std::free(shrunk);
}

TEST_F(AllocatorOverrideTest, ReallocNullptrBehavesLikeMalloc) {
    void *ptr = std::realloc(nullptr, 96);
    ASSERT_NE(ptr, nullptr);

    Span *span = PageCache::get_instance().map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 96u);

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, ReallocZeroFreesAllocationAndReturnsNull) {
    void *ptr = std::malloc(64);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(std::realloc(ptr, 0), nullptr);
}

TEST_F(AllocatorOverrideTest, OperatorNewIsTrackedByPageCache) {
    auto *ptr = new unsigned char[48];
    ASSERT_NE(ptr, nullptr);

    Span *span = PageCache::get_instance().map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 48u);

    delete[] ptr;
}

TEST_F(AllocatorOverrideTest, NothrowNewReturnsManagedPointer) {
    auto *ptr = new (std::nothrow) unsigned char[80];
    ASSERT_NE(ptr, nullptr);

    Span *span = PageCache::get_instance().map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_GE(span->obj_size, 80u);

    delete[] ptr;
}

TEST_F(AllocatorOverrideTest, AlignedAllocReturnsAlignedTrackedPointer) {
    void *ptr = ::aligned_alloc(64, 256);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0u);
    Span *span = PageCache::get_instance().try_map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_TRUE(span->is_use);

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, PosixMemalignReturnsAlignedTrackedPointer) {
    void *ptr = nullptr;
    ASSERT_EQ(::posix_memalign(&ptr, 128, 96), 0);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 128, 0u);
    Span *span = PageCache::get_instance().try_map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_TRUE(span->is_use);

    std::memset(ptr, 0x5a, 96);
    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, PosixMemalignRejectsInvalidAlignment) {
    void *ptr = reinterpret_cast<void *>(0x1);

    EXPECT_EQ(::posix_memalign(&ptr, 24, 96), EINVAL);
    EXPECT_EQ(ptr, reinterpret_cast<void *>(0x1));
}

TEST_F(AllocatorOverrideTest, ZeroSizeAllocationsRemainFreeable) {
    void *pointers[] = {std::malloc(0), std::calloc(0, 16),
                        std::realloc(nullptr, 0), ::pvalloc(0)};
    for (void *ptr : pointers) {
        ASSERT_NE(ptr, nullptr);
        std::free(ptr);
    }
}

TEST_F(AllocatorOverrideTest, OverflowReturnsEnomemAndPreservesReallocInput) {
    volatile size_t huge = std::numeric_limits<size_t>::max();
    errno = 0;
    EXPECT_EQ(std::malloc(huge), nullptr);
    EXPECT_EQ(errno, ENOMEM);
    errno = 0;
    EXPECT_EQ(std::calloc(huge, 2), nullptr);
    EXPECT_EQ(errno, ENOMEM);
    errno = 0;
    EXPECT_EQ(::memalign(64, huge), nullptr);
    EXPECT_EQ(errno, ENOMEM);
    errno = 0;
    EXPECT_EQ(::pvalloc(huge), nullptr);
    EXPECT_EQ(errno, ENOMEM);

    auto *ptr = static_cast<unsigned char *>(std::malloc(32));
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0x5a, 32);
    EXPECT_EQ(std::realloc(ptr, huge), nullptr);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(ptr[i], 0x5a);
    }
    std::free(ptr);
}

#if __SIZEOF_POINTER__ == 8
TEST_F(AllocatorOverrideTest, MmapFailureDoesNotTerminateOrLeavePageLockHeld) {
    volatile size_t huge = size_t(1) << 62;
    errno = 0;
    EXPECT_EQ(std::malloc(huge), nullptr);
    EXPECT_EQ(errno, ENOMEM);
    EXPECT_THROW((void)::operator new(huge), std::bad_alloc);
    EXPECT_EQ(::operator new(huge, std::nothrow), nullptr);
    EXPECT_THROW(zmalloc(huge), std::bad_alloc);

#if defined(__cpp_aligned_new)
    const auto alignment = std::align_val_t(65536);
    EXPECT_THROW((void)::operator new(huge, alignment), std::bad_alloc);
    EXPECT_EQ(::operator new(huge, alignment, std::nothrow), nullptr);
#endif

    void *ptr = std::malloc(MAX_BYTES + 1);
    ASSERT_NE(ptr, nullptr);
    std::free(ptr);
}
#endif

TEST_F(AllocatorOverrideTest, PosixMemalignFailurePreservesPointerAndErrno) {
    void *ptr = reinterpret_cast<void *>(0x1234);
    volatile size_t huge = std::numeric_limits<size_t>::max();
    errno = EBUSY;
    EXPECT_EQ(::posix_memalign(&ptr, 64, huge), ENOMEM);
    EXPECT_EQ(ptr, reinterpret_cast<void *>(0x1234));
    EXPECT_EQ(errno, EBUSY);
}

TEST_F(AllocatorOverrideTest, FreePreservesErrno) {
    void *ptr = std::malloc(2 * 1024 * 1024);
    ASSERT_NE(ptr, nullptr);
    errno = EBUSY;
    std::free(ptr);
    EXPECT_EQ(errno, EBUSY);
    std::free(nullptr);
    EXPECT_EQ(errno, EBUSY);
}

TEST_F(AllocatorOverrideTest, UsableSizeSupportsManagedAndAlignedAllocations) {
    EXPECT_EQ(::malloc_usable_size(nullptr), 0u);
    for (size_t size : {size_t(1), size_t(63), size_t(2 * 1024 * 1024)}) {
        void *ptr = std::malloc(size);
        ASSERT_NE(ptr, nullptr);
        EXPECT_GE(::malloc_usable_size(ptr), size);
        std::free(ptr);
    }
    void *ptr = nullptr;
    ASSERT_EQ(::posix_memalign(&ptr, 128, 96), 0);
    EXPECT_GE(::malloc_usable_size(ptr), 96u);
    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, ReallocWithinSizeClassRetainsPointerAndContents) {
    auto *ptr = static_cast<unsigned char *>(std::malloc(63));
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0x5a, 63);
    void *next = std::realloc(ptr, 64);
    ASSERT_EQ(next, ptr);
    next = std::realloc(next, 49);
    ASSERT_EQ(next, ptr);
    for (size_t i = 0; i < 49; ++i) {
        EXPECT_EQ(ptr[i], 0x5a);
    }
    std::free(next);
}

TEST_F(AllocatorOverrideTest, LargeAlignmentSupportsFreeReallocAndMappingCleanup) {
    constexpr size_t size = 2 * 1024 * 1024;
    constexpr size_t alignment = 65536;
    for (size_t i = 0; i < 8; ++i) {
        void *ptr = nullptr;
        ASSERT_EQ(::posix_memalign(&ptr, alignment, size), 0);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0u);
        EXPECT_GE(::malloc_usable_size(ptr), size);
        void *header_page = static_cast<char *>(ptr) - 1;
        auto &pc = PageCache::get_instance();
        ASSERT_NE(pc.try_map_object_to_span(header_page), nullptr);
        std::memset(ptr, 0x5a, size);
        if (i % 2 == 0) {
            void *next = std::realloc(ptr, size + 1024);
            ASSERT_NE(next, nullptr);
            const auto *bytes = static_cast<const unsigned char *>(next);
            EXPECT_EQ(bytes[0], 0x5a);
            EXPECT_EQ(bytes[size - 1], 0x5a);
            std::free(next);
        } else {
            std::free(ptr);
        }
        EXPECT_EQ(pc.try_map_object_to_span(header_page), nullptr);
    }
}

TEST_F(AllocatorOverrideTest, VallocReturnsPageAlignedPointer) {
    void *ptr = ::valloc(33);
    ASSERT_NE(ptr, nullptr);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % PAGE_SIZE, 0u);
    Span *span = PageCache::get_instance().try_map_object_to_span(ptr);
    ASSERT_NE(span, nullptr);
    EXPECT_TRUE(span->is_use);

    std::free(ptr);
}

#if defined(__GLIBC__)
TEST_F(AllocatorOverrideTest, UsableSizeSupportsForeignLibcAllocation) {
    void *ptr = __libc_malloc(96);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GE(::malloc_usable_size(ptr), 96u);
    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, FreeHandlesForeignLibcAllocation) {
    void *ptr = __libc_malloc(96);
    ASSERT_NE(ptr, nullptr);

    std::free(ptr);
}

TEST_F(AllocatorOverrideTest, ReallocHandlesForeignLibcAllocation) {
    auto *ptr = static_cast<unsigned char *>(__libc_malloc(32));
    ASSERT_NE(ptr, nullptr);
    for (size_t i = 0; i < 32; ++i) {
        ptr[i] = static_cast<unsigned char>(i + 3);
    }

    auto *grown = static_cast<unsigned char *>(std::realloc(ptr, 80));
    ASSERT_NE(grown, nullptr);
    EXPECT_EQ(PageCache::get_instance().try_map_object_to_span(grown), nullptr);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(grown[i], static_cast<unsigned char>(i + 3));
    }

    std::free(grown);
}
#endif

#if defined(__cpp_aligned_new)
TEST_F(AllocatorOverrideTest, StandardAlignedNewDeleteVariants) {
    const auto alignment = std::align_val_t(128);
    void *single = ::operator new(48, alignment);
    ASSERT_NE(single, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(single) % 128, 0u);
    ::operator delete(single, size_t(48), alignment);

    void *array = ::operator new[](256, alignment);
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(array) % 128, 0u);
    ::operator delete[](array, alignment);

    void *nothrow = ::operator new(64, alignment, std::nothrow);
    ASSERT_NE(nothrow, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(nothrow) % 128, 0u);
    ::operator delete(nothrow, alignment, std::nothrow);

    const auto small_alignment = std::align_val_t(alignof(std::max_align_t));
    void *small = ::operator new(24, small_alignment);
    ASSERT_NE(small, nullptr);
    ::operator delete(small, small_alignment);
}

TEST_F(AllocatorOverrideTest, OverAlignedObjectsAndArraysUseStandardDelete) {
    struct alignas(128) Value { int number = 42; };
    auto *value = new Value;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(value) % 128, 0u);
    EXPECT_EQ(value->number, 42);
    delete value;
    auto *values = new Value[3];
    EXPECT_EQ(reinterpret_cast<uintptr_t>(values) % 128, 0u);
    EXPECT_EQ(values[2].number, 42);
    delete[] values;
}
#endif

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
