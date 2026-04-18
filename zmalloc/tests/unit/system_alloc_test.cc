#include "zmalloc/internal/system_alloc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <new>

#include "zmalloc/internal/zmalloc_config.h"

namespace zmalloc {
namespace {

TEST(SystemAllocTest, AllocateAndFreeSinglePage) {
    void *p = system_alloc(1);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0u);
    system_free(p, 1);
}

TEST(SystemAllocTest, AllocateAndFreeMultiplePages) {
    void *p = system_alloc(8);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0u);
    system_free(p, 8);
}

TEST(SystemAllocTest, RepeatedAllocationsRemainAligned) {
    void *p1 = system_alloc(2);
    void *p2 = system_alloc(3);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % PAGE_SIZE, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % PAGE_SIZE, 0u);
    system_free(p1, 2);
    system_free(p2, 3);
}

TEST(SystemAllocTest, ZeroPagesThrowsBadAlloc) {
    EXPECT_THROW(system_alloc(0), std::bad_alloc);
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
