#include "znet/buffer.h"
#include "znet/socket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include <gtest/gtest.h>

#include "zco/sched.h"
#include "zco/wait_group.h"

namespace znet {
namespace {


class BufferUnitTest : public ::testing::Test {};

TEST_F(BufferUnitTest, AppendAndRetrieveWorks) {
    Buffer buffer;
    buffer.append("hello", 5);
    EXPECT_EQ(buffer.readable_bytes(), 5U);
    EXPECT_EQ(buffer.retrieve_as_string(2), "he");
    EXPECT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "llo");
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST_F(BufferUnitTest, AppendStringAndPeek) {
    Buffer buffer;
    buffer.append(std::string("abc"));
    ASSERT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(std::string(buffer.peek(), 3), "abc");
}

TEST_F(BufferUnitTest, ReusesPrependSpaceWithoutCorruptingReadableData) {
    Buffer buffer(8);
    buffer.append("12345678", 8);
    EXPECT_EQ(buffer.retrieve_as_string(5), "12345");

    buffer.append("abcd", 4);

    EXPECT_EQ(buffer.readable_bytes(), 7U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "678abcd");
}

TEST_F(BufferUnitTest, FindCrLfReturnsExpectedPointer) {
    Buffer with_crlf;
    with_crlf.append("abc\r\ndef", 8);
    const char *found = with_crlf.find_crlf();
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(std::string(with_crlf.peek(), found), "abc");

    Buffer without_crlf;
    without_crlf.append("abcdef", 6);
    EXPECT_EQ(without_crlf.find_crlf(), nullptr);
}

TEST_F(BufferUnitTest, RetrieveHandlesOutOfRangeLength) {
    Buffer buffer;
    buffer.append("xyz", 3);
    buffer.retrieve(10);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
    EXPECT_EQ(buffer.peek(), nullptr);
}

TEST_F(BufferUnitTest, AppendNullOrEmptyInputIsNoop) {
    Buffer buffer;
    buffer.append(static_cast<const void *>(nullptr), 5);
    buffer.append(static_cast<const char *>(nullptr), 5);
    buffer.append("", 0);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST_F(BufferUnitTest, RetrieveAsStringOnEmptyBufferReturnsEmptyString) {
    Buffer buffer;
    EXPECT_EQ(buffer.retrieve_as_string(8), "");
    EXPECT_EQ(buffer.retrieve_all_as_string(), "");
}

TEST_F(BufferUnitTest, ReadFromSocketValidatesArguments) {
    Buffer buffer;
    int saved_errno = 0;

    errno = 0;
    EXPECT_EQ(buffer.read_from_socket(nullptr, 16, 10, &saved_errno), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(saved_errno, EBADF);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto socket = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(socket, nullptr);

    errno = 0;
    EXPECT_EQ(buffer.read_from_socket(socket, 0, 10, &saved_errno), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(saved_errno, EINVAL);

    socket->close();
    ::close(pair[1]);
}

TEST_F(BufferUnitTest, WriteToSocketHandlesInvalidAndEmptyCases) {
    Buffer buffer;
    int saved_errno = 0;

    errno = 0;
    EXPECT_EQ(buffer.write_to_socket(nullptr, 10, &saved_errno), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(saved_errno, EBADF);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto socket = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(socket, nullptr);

    EXPECT_EQ(buffer.write_to_socket(socket, 10, &saved_errno), 0);

    socket->close();
    ::close(pair[1]);
}

TEST_F(BufferUnitTest, ReadFromSocketInvalidPathAllowsNullSavedErrno) {
    Buffer buffer;
    errno = 0;
    EXPECT_EQ(buffer.read_from_socket(nullptr, 8, 10, nullptr), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(BufferUnitTest, WriteToSocketInvalidPathAllowsNullSavedErrno) {
    Buffer buffer;
    errno = 0;
    EXPECT_EQ(buffer.write_to_socket(nullptr, 10, nullptr), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(BufferUnitTest, ReadAndWriteDetectClosedSocketObjectAsBadFd) {
    Buffer buffer;
    int saved_errno = 0;

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto socket = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(socket, nullptr);
    socket->close();

    errno = 0;
    EXPECT_EQ(buffer.read_from_socket(socket, 8, 10, &saved_errno), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(saved_errno, EBADF);

    buffer.append("x", 1);
    errno = 0;
    EXPECT_EQ(buffer.write_to_socket(socket, 10, &saved_errno), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(saved_errno, EBADF);

    ::close(pair[1]);
}

TEST_F(BufferUnitTest, ReadTimeoutStoresSavedErrnoWhenReadFails) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input;
    zco::WaitGroup done(1);
    std::atomic<int> captured_errno{0};
    std::atomic<int> saved_errno{0};
    zco::go([&]() {
        int local_saved_errno = 0;
        errno = 0;
        EXPECT_EQ(input.read_from_socket(reader, 4, 10, &local_saved_errno),
                  -1);
        captured_errno.store(errno, std::memory_order_release);
        saved_errno.store(local_saved_errno, std::memory_order_release);
        done.done();
    });
    done.wait();

    const int err = captured_errno.load(std::memory_order_acquire);
    const int saved = saved_errno.load(std::memory_order_acquire);
    EXPECT_TRUE(err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK);
    EXPECT_EQ(saved, err);

    reader->close();
    ::close(pair[1]);
    zco::shutdown();
}

TEST_F(BufferUnitTest, ReadAndWriteSocketPathWorksInCoroutineContext) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input;
    Buffer output;
    output.append("hello", 5);

    zco::WaitGroup done(1);
    zco::go([&]() {
        int saved_errno = 0;
        EXPECT_EQ(::send(pair[1], "hello", 5, 0), 5);
        EXPECT_EQ(input.read_from_socket(reader, 5, 200, &saved_errno), 5);
        char recvbuf[8] = {0};
        EXPECT_EQ(output.write_to_socket(reader, 200, nullptr), 5);
        EXPECT_EQ(::recv(pair[1], recvbuf, sizeof(recvbuf), 0), 5);
        EXPECT_STREQ(recvbuf, "hello");
        done.done();
    });
    done.wait();

    EXPECT_EQ(input.retrieve_all_as_string(), "hello");
    reader->close();
    ::close(pair[1]);
    zco::shutdown();
}

TEST_F(BufferUnitTest, ReadFromSocketDoesNotPreGrowToMaxReadBytes) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input(8);
    input.append("abcd", 4);
    EXPECT_EQ(input.retrieve_as_string(4), "abcd");
    const size_t writable_before = input.writable_bytes();

    zco::WaitGroup done(1);
    zco::go([&]() {
        int saved_errno = 0;
        EXPECT_EQ(::send(pair[1], "xy", 2, 0), 2);
        EXPECT_EQ(input.read_from_socket(reader, 64 * 1024, 200,
                                         &saved_errno),
                  2);
        done.done();
    });
    done.wait();

    EXPECT_EQ(input.writable_bytes(), writable_before - 2);
    EXPECT_EQ(input.retrieve_all_as_string(), "xy");

    reader->close();
    ::close(pair[1]);
    zco::shutdown();
}

} // namespace
} // namespace znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
