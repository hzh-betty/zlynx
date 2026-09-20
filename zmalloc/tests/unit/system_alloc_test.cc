#include "zmalloc/internal/system_alloc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <new>
#include <vector>

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

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
