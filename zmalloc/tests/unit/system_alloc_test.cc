#include "zmalloc/internal/system_alloc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <limits>
#include <new>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "zmalloc/internal/zmalloc_config.h"

namespace zmalloc {
namespace {


class SystemAllocTest : public ::testing::Test {};

TEST_F(SystemAllocTest, AllocateAndFreeSinglePage) {
    void *p = system_alloc(1);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0u);
    system_free(p, 1);
}

TEST_F(SystemAllocTest, AllocateAndFreeMultiplePages) {
    void *p = system_alloc(8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0u);
    system_free(p, 8);
}

TEST_F(SystemAllocTest, RepeatedAllocationsRemainAligned) {
    void *p1 = system_alloc(2);
    void *p2 = system_alloc(3);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % PAGE_SIZE, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % PAGE_SIZE, 0u);
    system_free(p1, 2);
    system_free(p2, 3);
}

TEST_F(SystemAllocTest, RejectsOverflowBeforeMapping) {
    EXPECT_THROW(system_alloc(std::numeric_limits<size_t>::max()),
                 std::bad_alloc);
    EXPECT_THROW(system_alloc(std::numeric_limits<size_t>::max() / PAGE_SIZE),
                 std::bad_alloc);
}

TEST_F(SystemAllocTest, UnmappableRequestThrowsBadAlloc) {
    // 可通过溢出检查，但几乎覆盖整个 size_t 地址空间，内核必须拒绝映射。
    const size_t pages =
        (std::numeric_limits<size_t>::max() - PAGE_SIZE) / PAGE_SIZE;
    EXPECT_THROW(system_alloc(pages), std::bad_alloc);
}

TEST_F(SystemAllocTest, TrimmedMappingsRemainDisjointAndWritable) {
    std::vector<unsigned char *> blocks;
    for (size_t i = 1; i <= 64; ++i) {
        auto *p = static_cast<unsigned char *>(system_alloc(i));
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0u);
        p[0] = static_cast<unsigned char>(i);
        p[i * PAGE_SIZE - 1] = static_cast<unsigned char>(i);
        blocks.push_back(p);
    }
    for (size_t i = 1; i <= blocks.size(); ++i) {
        EXPECT_EQ(blocks[i - 1][0], i);
        EXPECT_EQ(blocks[i - 1][i * PAGE_SIZE - 1], i);
        system_free(blocks[i - 1], i);
    }
}

TEST_F(SystemAllocTest, ZeroPagesThrowsBadAlloc) {
    EXPECT_THROW(system_alloc(0), std::bad_alloc);
}

TEST_F(SystemAllocTest, NothrowEntryReportsFailureAndRemainsUsable) {
    errno = 0;
    EXPECT_EQ(system_alloc_nothrow(0), nullptr);
    EXPECT_EQ(errno, ENOMEM);
    EXPECT_EQ(system_alloc_nothrow(std::numeric_limits<size_t>::max()), nullptr);
    const size_t pages =
        (std::numeric_limits<size_t>::max() - PAGE_SIZE) / PAGE_SIZE;
    EXPECT_EQ(system_alloc_nothrow(pages), nullptr);

    auto *ptr = static_cast<unsigned char *>(system_alloc_nothrow(1));
    ASSERT_NE(ptr, nullptr);
    ptr[0] = 0x5a;
    ptr[PAGE_SIZE - 1] = 0xa5;
    EXPECT_EQ(ptr[0], 0x5a);
    EXPECT_EQ(ptr[PAGE_SIZE - 1], 0xa5);
    system_free(ptr, 1);
}

TEST_F(SystemAllocTest, ReleaseDiscardsPhysicalPagesAndKeepsAddressWritable) {
    auto *ptr = static_cast<unsigned char *>(system_alloc(2));
    std::memset(ptr, 0x5a, 2 * PAGE_SIZE);
    const size_t os_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> resident(2 * PAGE_SIZE / os_page);
    ASSERT_EQ(mincore(ptr, 2 * PAGE_SIZE, resident.data()), 0);
    for (unsigned char page : resident) EXPECT_EQ(page & 1, 1);

    ASSERT_TRUE(system_release(ptr, 2));
    // mincore 检查驻留页，避免用进程 RSS 的采样噪声判断回收是否成功。
    ASSERT_EQ(mincore(ptr, 2 * PAGE_SIZE, resident.data()), 0);
    for (unsigned char page : resident) EXPECT_EQ(page & 1, 0);
    for (size_t i = 0; i < 2 * PAGE_SIZE; ++i) EXPECT_EQ(ptr[i], 0);
    std::memset(ptr, 0xa5, 2 * PAGE_SIZE);
    EXPECT_EQ(ptr[0], 0xa5);
    EXPECT_EQ(ptr[2 * PAGE_SIZE - 1], 0xa5);
    system_free(ptr, 2);
}

TEST_F(SystemAllocTest, ReleaseRejectsInvalidRanges) {
    void *ptr = system_alloc(1);
    EXPECT_FALSE(system_release(nullptr, 1));
    EXPECT_FALSE(system_release(ptr, 0));
    EXPECT_FALSE(system_release(static_cast<char *>(ptr) + 1, 1));
    EXPECT_FALSE(system_release(ptr, std::numeric_limits<size_t>::max()));
    const uintptr_t last_page =
        std::numeric_limits<uintptr_t>::max() & ~(uintptr_t(PAGE_SIZE) - 1);
    EXPECT_FALSE(system_release(reinterpret_cast<void *>(last_page), 1));
    // 已解除的映射由内核拒绝，不能报告为成功回收。
    system_free(ptr, 1);
    EXPECT_FALSE(system_release(ptr, 1));
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
