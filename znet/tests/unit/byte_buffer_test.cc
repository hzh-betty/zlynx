#include "znet/byte_buffer.h"
#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>

using namespace znet;

TEST(ByteBufferTest, EmptyRangesAndCrLfAreSafe) {
    ByteBuffer buffer(0);
    EXPECT_EQ(buffer.find_crlf(), nullptr);
    EXPECT_TRUE(buffer.view().empty());
    buffer.append("abc\r\ndef");
    ASSERT_NE(buffer.find_crlf(), nullptr);
    EXPECT_EQ(buffer.find_crlf() - buffer.peek(), 3);
    EXPECT_EQ(buffer.retrieve_as_string(5), "abc\r\n");
    EXPECT_EQ(buffer.retrieve_all_as_string(), "def");
    EXPECT_EQ(buffer.find_crlf(), nullptr);
    buffer.retrieve(100);
    EXPECT_TRUE(buffer.view().empty());
}

TEST(ByteBufferTest, CompactsGrowsAndSupportsSelfAppend) {
    ByteBuffer buffer(8);
    buffer.append("12345678");
    buffer.retrieve(5);
    buffer.append("abcd");
    EXPECT_EQ(buffer.view(), "678abcd");
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.retrieve_all_as_string(), "678abcd678abcd");
    buffer.append("abcdefgh");
    buffer.retrieve(5);
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.view(), "fghfgh");
}

TEST(ByteBufferTest, CommitAndAppendValidateMemoryContract) {
    ByteBuffer buffer(2);
    EXPECT_THROW(buffer.append(nullptr, 1), std::invalid_argument);
    EXPECT_NO_THROW(buffer.append(nullptr, 0));
    EXPECT_THROW(buffer.has_written(3), std::out_of_range);
    buffer.ensure_writable_bytes(3);
    std::copy_n("abc", 3, buffer.begin_write());
    buffer.has_written(3);
    EXPECT_EQ(buffer.view(), "abc");
    EXPECT_THROW(buffer.ensure_writable_bytes(SIZE_MAX), std::length_error);
    EXPECT_EQ(buffer.view(), "abc");
}

TEST(ByteBufferTest, SmallPartialConsumptionRetainsWritableSlack) {
    constexpr size_t window = 4096;
    ByteBuffer buffer(window);
    buffer.append(std::string(window, 'x'));
    buffer.retrieve(16);
    buffer.append(std::string(16, 'x'));
    // One small consume/append must leave room for subsequent small writes.
    EXPECT_GE(buffer.writable_bytes(), window / 2);
    EXPECT_LE(buffer.readable_bytes() + buffer.writable_bytes(), 2 * window);
}

TEST(ByteBufferTest, SlidingWindowPreservesBytesAcrossGrowthAndCompaction) {
    ByteBuffer buffer(64);
    std::string expected(64, 'x');
    buffer.append(expected);
    for (int i = 0; i < 512; ++i) {
        buffer.retrieve(16);
        expected.erase(0, 16);
        const std::string block(16, static_cast<char>('a' + i % 26));
        buffer.ensure_writable_bytes(block.size());
        std::copy(block.begin(), block.end(), buffer.begin_write());
        buffer.has_written(block.size());
        expected += block;
        ASSERT_EQ(buffer.view(), expected) << i;
    }
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.view(), expected + expected);
    EXPECT_THROW(buffer.append(buffer.begin_write() - 1,
                               buffer.writable_bytes() + 2),
                 std::out_of_range);
    EXPECT_EQ(buffer.view(), expected + expected);
}
