#define private public
#include "async/buffer.h"
#undef private
#include <cstring>
#include <gtest/gtest.h>
#include <new>
#include <string>
#include <vector>

using namespace zlog;
using namespace zlog::detail;

class BufferTest : public ::testing::Test {
  protected:
    void SetUp() override {}
    void TearDown() override {}
    Buffer buf;
};

TEST_F(BufferTest, InitialState) {
    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_EQ(buf.writable_size(), kDefaultBufferSize);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), kDefaultBufferSize);
    EXPECT_NE(buf.begin(), static_cast<const char *>(NULL));
}

TEST_F(BufferTest, DefaultBufferConstants) {
    EXPECT_EQ(kDefaultBufferSize, 1024u * 1024u * 2u);   // 2MB
    EXPECT_EQ(kThresholdBufferSize, 1024u * 1024u * 8u); // 8MB
    EXPECT_EQ(kMaxBufferSize, 1024u * 1024u * 512u);     // 512MB
}

TEST_F(BufferTest, PushSmallData) {
    const char *data = "hello";
    buf.push(data, 5);

    EXPECT_EQ(buf.readable_size(), 5u);
    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(std::string(buf.begin(), buf.readable_size()), "hello");
}

TEST_F(BufferTest, PushEmptyData) {
    buf.push("", 0);

    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, PushSingleByte) {
    buf.push("X", 1);

    EXPECT_EQ(buf.readable_size(), 1u);
    EXPECT_EQ(buf.begin()[0], 'X');
}

TEST_F(BufferTest, PushLargeData) {
    std::vector<char> largeData(1024 * 1024, 'A'); // 1MB
    buf.push(largeData.data(), largeData.size());

    EXPECT_EQ(buf.readable_size(), largeData.size());
    EXPECT_EQ(std::memcmp(buf.begin(), largeData.data(), largeData.size()), 0);
}

TEST_F(BufferTest, PushMultipleTimes) {
    buf.push("hello", 5);
    buf.push(" ", 1);
    buf.push("world", 5);

    EXPECT_EQ(buf.readable_size(), 11u);
    EXPECT_EQ(std::string(buf.begin(), buf.readable_size()), "hello world");
}

TEST_F(BufferTest, PushBinaryData) {
    char binary[] = {0x00, 0x01, 0x02, static_cast<char>(0xFF),
                     static_cast<char>(0xFE)};
    buf.push(binary, 5);

    EXPECT_EQ(buf.readable_size(), 5u);
    EXPECT_EQ(std::memcmp(buf.begin(), binary, 5), 0);
}

TEST_F(BufferTest, PushWithNullBytes) {
    const char data[] = "hello\0world";
    buf.push(data, 11);

    EXPECT_EQ(buf.readable_size(), 11u);
    EXPECT_EQ(std::memcmp(buf.begin(), data, 11), 0);
}





TEST_F(BufferTest, ResetAfterPush) {
    buf.push("hello world", 11);
    buf.reset();

    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.writable_size(), buf.capacity());
}


TEST_F(BufferTest, ResetOnEmpty) {
    buf.reset();

    EXPECT_EQ(buf.readable_size(), 0u);
    EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, PushAfterReset) {
    buf.push("first", 5);
    buf.reset();
    buf.push("second", 6);

    EXPECT_EQ(buf.readable_size(), 6u);
    EXPECT_EQ(std::string(buf.begin(), buf.readable_size()), "second");
}

TEST_F(BufferTest, SwapBothNonEmpty) {
    Buffer b2;
    buf.push("hello", 5);
    b2.push("world", 5);

    buf.swap(b2);

    EXPECT_EQ(std::string(buf.begin(), buf.readable_size()), "world");
    EXPECT_EQ(std::string(b2.begin(), b2.readable_size()), "hello");
}

TEST_F(BufferTest, SwapWithEmpty) {
    Buffer b2;
    buf.push("hello", 5);

    buf.swap(b2);

    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(std::string(b2.begin(), b2.readable_size()), "hello");
}

TEST_F(BufferTest, SwapBothEmpty) {
    Buffer b2;
    buf.swap(b2);

    EXPECT_TRUE(buf.empty());
    EXPECT_TRUE(b2.empty());
}

TEST_F(BufferTest, SwapDifferentSizes) {
    Buffer b2;
    std::vector<char> largeData(1024 * 1024 * 3, 'X'); // 3MB，触发扩容
    buf.push(largeData.data(), largeData.size());
    b2.push("small", 5);

    size_t bufCap = buf.capacity();
    size_t b2Cap = b2.capacity();

    buf.swap(b2);

    EXPECT_EQ(buf.capacity(), b2Cap);
    EXPECT_EQ(b2.capacity(), bufCap);
    EXPECT_EQ(buf.readable_size(), 5u);
    EXPECT_EQ(b2.readable_size(), largeData.size());
}

TEST_F(BufferTest, SwapSelf) {
    buf.push("hello", 5);
    buf.swap(buf);

    EXPECT_EQ(std::string(buf.begin(), buf.readable_size()), "hello");
}

TEST_F(BufferTest, AutoResizeSmall) {
    std::vector<char> data(kDefaultBufferSize + 1, 'A');
    buf.push(data.data(), data.size());

    EXPECT_GT(buf.capacity(), kDefaultBufferSize);
    EXPECT_EQ(buf.readable_size(), data.size());
}

TEST_F(BufferTest, AutoResizeLarge) {
    std::vector<char> data(1024 * 1024 * 3, 'A'); // 3MB > 2MB
    EXPECT_NO_THROW(buf.push(data.data(), data.size()));
    EXPECT_EQ(buf.readable_size(), data.size());
}

TEST_F(BufferTest, ResizeBelowThreshold) {
    // 低于阈值时容量翻倍
    size_t origCap = buf.capacity();
    std::vector<char> data(origCap + 1, 'X');
    buf.push(data.data(), data.size());

    // 新容量至少翻倍，并确保足够容纳全部已有数据与新数据
    EXPECT_GE(buf.capacity(), origCap * 2);
}

TEST_F(BufferTest, GeometricGrowthPreservesDataPastEightMiB) {
    std::string chunk(1024 * 1024, '\0');
    for (int i = 0; i < 32; ++i) {
        chunk.assign(chunk.size(), static_cast<char>(i));
        buf.push(chunk.data(), chunk.size());
    }
    EXPECT_GE(buf.capacity(), 32u * 1024u * 1024u);
    EXPECT_LT(buf.capacity(), 48u * 1024u * 1024u);
    EXPECT_EQ(buf.readable_size(), 32u * chunk.size());
    for (int i = 0; i < 32; ++i) {
        chunk.assign(chunk.size(), static_cast<char>(i));
        EXPECT_EQ(std::memcmp(buf.begin() + i * chunk.size(), chunk.data(), chunk.size()), 0);
    }
}

TEST_F(BufferTest, GrowthFromExactEightMiBBoundary) {
    buf.reserve(kThresholdBufferSize);
    const std::string data(kThresholdBufferSize, 'x');
    buf.push(data.data(), data.size());
    buf.push("y", 1);
    EXPECT_EQ(buf.capacity(), kThresholdBufferSize + kThresholdBufferSize / 2);
    EXPECT_EQ(std::memcmp(buf.begin(), data.data(), data.size()), 0);
    EXPECT_EQ(buf.begin()[data.size()], 'y');
}

TEST_F(BufferTest, GrowthIsCappedAtMaximum) {
    // 只检查容量计算，避免为边界测试实际分配 512 MiB。
    buf.capacity_ = kMaxBufferSize * 2 / 3 + 1;
    buf.writer_idx_ = buf.capacity_;
    EXPECT_EQ(buf.calculate_new_size(1), kMaxBufferSize);
    buf.capacity_ = kMaxBufferSize;
    buf.writer_idx_ = kMaxBufferSize - 1;
    EXPECT_EQ(buf.calculate_new_size(1), kMaxBufferSize);
    EXPECT_TRUE(buf.can_accommodate(1));
    EXPECT_FALSE(buf.can_accommodate(2));
}

TEST_F(BufferTest, CapacityInitial) {
    EXPECT_EQ(buf.capacity(), kDefaultBufferSize);
}

TEST_F(BufferTest, CapacityAfterPush) {
    buf.push("hello", 5);
    EXPECT_EQ(buf.capacity(), kDefaultBufferSize); // 不扩容
}

TEST_F(BufferTest, CapacityAfterResize) {
    std::vector<char> largeData(kDefaultBufferSize + 1, 'X');
    buf.push(largeData.data(), largeData.size());
    EXPECT_GT(buf.capacity(), kDefaultBufferSize);
}

TEST_F(BufferTest, CapacityUnchangedAfterReset) {
    std::vector<char> largeData(kDefaultBufferSize + 1, 'X');
    buf.push(largeData.data(), largeData.size());
    size_t expandedCap = buf.capacity();

    buf.reset();

    EXPECT_EQ(buf.capacity(), expandedCap); // reset不缩减容量
}

TEST_F(BufferTest, WriteAbleSizeInitial) {
    EXPECT_EQ(buf.writable_size(), kDefaultBufferSize);
}

TEST_F(BufferTest, WriteAbleSizeAfterPush) {
    buf.push("hello", 5);
    EXPECT_EQ(buf.writable_size(), kDefaultBufferSize - 5);
}

TEST_F(BufferTest, WriteAbleSizeAfterReset) {
    buf.push("hello", 5);
    buf.reset();
    EXPECT_EQ(buf.writable_size(), buf.capacity());
}

TEST_F(BufferTest, PushExactCapacity) {
    std::vector<char> data(kDefaultBufferSize, 'X');
    buf.push(data.data(), data.size());

    EXPECT_EQ(buf.readable_size(), kDefaultBufferSize);
    EXPECT_EQ(buf.writable_size(), 0u);
}

TEST_F(BufferTest, BeginPointerConsistency) {
    buf.push("hello", 5);
    const char *p1 = buf.begin();

    buf.push(" world", 6);
    const char *p2 = buf.begin();

    // 不扩容时，begin指针不变
    EXPECT_EQ(p1, p2);
}


TEST_F(BufferTest, ManySmallPushes) {
    const int iterations = 10000;
    for (int i = 0; i < iterations; i++) {
        buf.push("x", 1);
    }

    EXPECT_EQ(buf.readable_size(), static_cast<size_t>(iterations));
}

TEST_F(BufferTest, PushResetPushCycle) {
    for (int cycle = 0; cycle < 100; cycle++) {
        buf.push("test", 4);
        buf.reset();
    }

    EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, ResetCycle) {
    for (int cycle = 0; cycle < 100; cycle++) {
        buf.push("test data", 9);
        buf.reset();
    }

    EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, DataIntegrityLarge) {
    std::vector<char> original(1024 * 512, '\0'); // 512KB
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<char>(i % 256);
    }

    buf.push(original.data(), original.size());

    EXPECT_EQ(buf.readable_size(), original.size());
    EXPECT_EQ(std::memcmp(buf.begin(), original.data(), original.size()), 0);
}

TEST_F(BufferTest, DataIntegrityAfterResize) {
    // 先填充一些数据
    buf.push("prefix_", 7);

    // 触发扩容
    std::vector<char> largeData(kDefaultBufferSize, 'Y');
    buf.push(largeData.data(), largeData.size());

    // 验证prefix仍在
    EXPECT_EQ(std::memcmp(buf.begin(), "prefix_", 7), 0);
}

TEST_F(BufferTest, CanAccommodateWhenResizeIsNeeded) {
    const size_t len = buf.writable_size() + 1;
    EXPECT_TRUE(buf.can_accommodate(len));
}

TEST_F(BufferTest, CanAccommodateReturnsFalseWhenBeyondMax) {
    const size_t len = kMaxBufferSize + 1;
    EXPECT_FALSE(buf.can_accommodate(len));
}

TEST_F(BufferTest, ExactMaximumFitsAndOverflowIsRejectedBeforeCopying) {
    EXPECT_TRUE(buf.can_accommodate(kMaxBufferSize));
    EXPECT_EQ(buf.calculate_new_size(kMaxBufferSize), kMaxBufferSize);
    EXPECT_THROW(buf.push("x", static_cast<size_t>(-1)), std::length_error);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), kDefaultBufferSize);
}


TEST_F(BufferTest, EnsureEnoughSizeRejectsBeyondMaxWithoutChangingState) {
    const size_t old_cap = kMaxBufferSize - 10;
    buf.capacity_ = old_cap;
    buf.writer_idx_ = old_cap;

    EXPECT_THROW(buf.ensure_enough_size(20), std::length_error);

    EXPECT_EQ(buf.capacity_, old_cap);
    EXPECT_EQ(buf.writer_idx_, old_cap);
}

TEST_F(BufferTest, EnsureEnoughSizeCappedBranchReallocPath) {
    buf.capacity_ = kMaxBufferSize - 100;
    buf.writer_idx_ = buf.capacity_ - 50;

    try {
        buf.ensure_enough_size(120);
        EXPECT_EQ(buf.capacity_, kMaxBufferSize);
    } catch (const std::bad_alloc &) {
        SUCCEED();
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
