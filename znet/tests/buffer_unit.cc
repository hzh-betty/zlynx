#include "znet/buffer.h"

#include <gtest/gtest.h>

namespace znet {
namespace {

TEST(BufferUnitTest, AppendAndRetrieveWorks) {
    Buffer buffer;
    buffer.append("hello", 5);
    EXPECT_EQ(buffer.readable_bytes(), 5U);
    EXPECT_EQ(buffer.retrieve_as_string(2), "he");
    EXPECT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "llo");
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST(BufferUnitTest, AppendStringAndPeek) {
    Buffer buffer;
    buffer.append(std::string("abc"));
    ASSERT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(std::string(buffer.peek(), 3), "abc");
}

TEST(BufferUnitTest, ReusesPrependSpaceWithoutCorruptingReadableData) {
    Buffer buffer(8);
    buffer.append("12345678", 8);
    EXPECT_EQ(buffer.retrieve_as_string(5), "12345");

    buffer.append("abcd", 4);

    EXPECT_EQ(buffer.readable_bytes(), 7U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "678abcd");
}

} // namespace
} // namespace znet
