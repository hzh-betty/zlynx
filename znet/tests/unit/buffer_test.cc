/**
 * @file buffer_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include "znet/buffer.h"
#include "znet/socket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include <gtest/gtest.h>

#include "znet/znet_logger.h"

#include "zco/coroutine.h"
#include "zco/sync/wait_group.h"

namespace znet {
namespace {


class BufferUnitTest : public ::testing::Test {};

TEST_F(BufferUnitTest, AppendAndRetrieveWorks) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    buffer.append("hello", 5);
    EXPECT_EQ(buffer.readable_bytes(), 5U);
    EXPECT_EQ(buffer.retrieve_as_string(2), "he");
    EXPECT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "llo");
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST_F(BufferUnitTest, AppendStringAndPeek) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    buffer.append(std::string("abc"));
    ASSERT_EQ(buffer.readable_bytes(), 3U);
    EXPECT_EQ(std::string(buffer.peek(), 3), "abc");
}

TEST_F(BufferUnitTest, ReusesPrependSpaceWithoutCorruptingReadableData) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer(8);
    buffer.append("12345678", 8);
    EXPECT_EQ(buffer.retrieve_as_string(5), "12345");

    buffer.append("abcd", 4);

    EXPECT_EQ(buffer.readable_bytes(), 7U);
    EXPECT_EQ(buffer.retrieve_all_as_string(), "678abcd");
}

TEST_F(BufferUnitTest, FindCrLfReturnsExpectedPointer) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
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
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    buffer.append("xyz", 3);
    buffer.retrieve(10);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
    EXPECT_EQ(buffer.peek(), nullptr);
}

TEST_F(BufferUnitTest, AppendNullOrEmptyInputIsNoop) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    buffer.append(static_cast<const void *>(nullptr), 5);
    buffer.append(static_cast<const char *>(nullptr), 5);
    buffer.append("", 0);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST_F(BufferUnitTest, RetrieveAsStringOnEmptyBufferReturnsEmptyString) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    EXPECT_EQ(buffer.retrieve_as_string(8), "");
    EXPECT_EQ(buffer.retrieve_all_as_string(), "");
}

TEST_F(BufferUnitTest, ReadFromSocketValidatesArguments) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
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
    zco::Runtime runtime(zco::RuntimeOptions{2});
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
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    errno = 0;
    EXPECT_EQ(buffer.read_from_socket(nullptr, 8, 10, nullptr), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(BufferUnitTest, WriteToSocketInvalidPathAllowsNullSavedErrno) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    Buffer buffer;
    errno = 0;
    EXPECT_EQ(buffer.write_to_socket(nullptr, 10, nullptr), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(BufferUnitTest, ReadAndWriteDetectClosedSocketObjectAsBadFd) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
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
    zco::Runtime runtime(zco::RuntimeOptions{1});

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input;
    zco::WaitGroup done(1);
    std::atomic<int> captured_errno{0};
    std::atomic<int> saved_errno{0};
    runtime.spawn([&]() {
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
    runtime.request_stop();
    runtime.join();
}

TEST_F(BufferUnitTest, ReadAndWriteSocketPathWorksInCoroutineContext) {
    zco::Runtime runtime(zco::RuntimeOptions{1});

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input;
    Buffer output;
    output.append("hello", 5);

    zco::WaitGroup done(1);
    runtime.spawn([&]() {
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
    runtime.request_stop();
    runtime.join();
}

TEST_F(BufferUnitTest, ReadFromSocketPreGrowsWritableSpace) {
    zco::Runtime runtime(zco::RuntimeOptions{1});

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    ASSERT_NE(reader, nullptr);

    Buffer input(8);
    input.append("abcd", 4);
    EXPECT_EQ(input.retrieve_as_string(4), "abcd");

    zco::WaitGroup done(1);
    runtime.spawn([&]() {
        int saved_errno = 0;
        EXPECT_EQ(::send(pair[1], "xy", 2, 0), 2);
        EXPECT_EQ(input.read_from_socket(reader, 64 * 1024, 200,
                                         &saved_errno),
                  2);
        done.done();
    });
    done.wait();

    EXPECT_GE(input.writable_bytes(), 64 * 1024 - 2);
    EXPECT_EQ(input.retrieve_all_as_string(), "xy");

    reader->close();
    ::close(pair[1]);
    runtime.request_stop();
    runtime.join();
}

TEST_F(BufferUnitTest, DirectReadPreservesPrefixHonorsLimitAndHandlesEof) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto reader = std::make_shared<Socket>(pair[0]);
    Buffer input(2);
    input.append("old", 3);
    zco::WaitGroup done(1);
    runtime.spawn([&]() {
        int saved_errno = 0;
        EXPECT_EQ(::send(pair[1], "abcdef", 6, 0), 6);
        EXPECT_EQ(input.read_from_socket(reader, 2, 0, &saved_errno), 2);
        EXPECT_EQ(input.retrieve_all_as_string(), "oldab");
        EXPECT_EQ(input.read_from_socket(reader, 4, 200, &saved_errno), 4);
        EXPECT_EQ(input.retrieve_all_as_string(), "cdef");
        EXPECT_EQ(::shutdown(pair[1], SHUT_WR), 0);
        EXPECT_EQ(input.read_from_socket(reader, 4, 200, &saved_errno), 0);
        EXPECT_EQ(input.readable_bytes(), 0u);
        done.done();
    });
    done.wait();
    reader->close();
    ::close(pair[1]);
    runtime.request_stop();
    runtime.join();
}

} // namespace
} // namespace znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    znet::init_logger();
    return RUN_ALL_TESTS();
}
