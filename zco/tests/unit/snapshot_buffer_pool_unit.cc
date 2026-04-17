#include <gtest/gtest.h>
#include <vector>

#include "support/test_fixture.h"
#include "zco/internal/snapshot_buffer_pool.h"

namespace zco {
namespace {

class SnapshotBufferPoolUnitTest : public test::RuntimeTestBase {};

TEST_F(SnapshotBufferPoolUnitTest, AcquireZeroSizeReturnsNullAndDynamicBucket) {
    SnapshotBufferPool pool;

    size_t capacity = 123;
    uint8_t bucket = 0;
    char *buffer = pool.acquire(0, &capacity, &bucket);

    EXPECT_EQ(buffer, nullptr);
    EXPECT_EQ(capacity, 0u);
    EXPECT_EQ(bucket, SnapshotBufferPool::dynamic_bucket_index());
}

TEST_F(SnapshotBufferPoolUnitTest, BucketBufferIsReusedAfterRelease) {
    SnapshotBufferPool pool;

    size_t capacity1 = 0;
    uint8_t bucket1 = 0;
    char *b1 = pool.acquire(3000, &capacity1, &bucket1);
    ASSERT_NE(b1, nullptr);
    EXPECT_NE(bucket1, SnapshotBufferPool::dynamic_bucket_index());

    pool.release(b1, bucket1, capacity1);

    size_t capacity2 = 0;
    uint8_t bucket2 = 0;
    char *b2 = pool.acquire(3000, &capacity2, &bucket2);
    ASSERT_NE(b2, nullptr);

    EXPECT_EQ(capacity2, capacity1);
    EXPECT_EQ(bucket2, bucket1);
    EXPECT_EQ(b2, b1);

    pool.release(b2, bucket2, capacity2);
}

TEST_F(SnapshotBufferPoolUnitTest,
       DynamicBufferUsesExactCapacityAndSafeRelease) {
    SnapshotBufferPool pool;

    constexpr size_t kLarge = 700 * 1024;
    size_t capacity = 0;
    uint8_t bucket = 0;
    char *buffer = pool.acquire(kLarge, &capacity, &bucket);

    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(bucket, SnapshotBufferPool::dynamic_bucket_index());
    EXPECT_EQ(capacity, kLarge);

    pool.release(buffer, bucket, capacity);
}

TEST_F(SnapshotBufferPoolUnitTest,
       ReleaseWithMismatchedCapacityFallsBackToDeletePath) {
    SnapshotBufferPool pool;

    size_t capacity = 0;
    uint8_t bucket = 0;
    char *buffer = pool.acquire(3000, &capacity, &bucket);
    ASSERT_NE(buffer, nullptr);

    pool.release(buffer, bucket, capacity + 1);
}

TEST_F(SnapshotBufferPoolUnitTest, ReleaseWithInvalidBucketIndexDeletesBuffer) {
    SnapshotBufferPool pool;

    char *raw = new char[64];
    pool.release(raw, static_cast<uint8_t>(250), 64);
}

TEST_F(SnapshotBufferPoolUnitTest, PerBucketLimitDropsOverflowBuffers) {
    SnapshotBufferPool pool;

    size_t capacity = 0;
    uint8_t bucket = 0;
    char *seed = pool.acquire(1024, &capacity, &bucket);
    ASSERT_NE(seed, nullptr);

    // Keep exactly one canonical buffer in pool for later reuse check.
    pool.release(seed, bucket, capacity);

    std::vector<char *> buffers;
    buffers.reserve(300);
    for (int i = 0; i < 300; ++i) {
        size_t cap = 0;
        uint8_t b = 0;
        char *buf = pool.acquire(1024, &cap, &b);
        ASSERT_NE(buf, nullptr);
        ASSERT_EQ(cap, capacity);
        ASSERT_EQ(b, bucket);
        buffers.push_back(buf);
    }

    for (char *buf : buffers) {
        pool.release(buf, bucket, capacity);
    }
}

} // namespace
} // namespace zco
