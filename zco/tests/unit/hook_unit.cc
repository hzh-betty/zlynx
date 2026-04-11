#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/hook.h"

namespace zco {
namespace {

class HookUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(HookUnitByHeaderTest, SocketPairReadWriteRoundTrip) {
    init(2);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    WaitGroup done(2);

    go([&done, pair]() {
        const char *message = "ok";
        EXPECT_EQ(co_write(pair[0], message, 2, 200), 2);
        done.done();
    });

    go([&done, pair]() {
        char out[8] = {0};
        EXPECT_EQ(co_read(pair[1], out, 2, 200), 2);
        EXPECT_STREQ(out, "ok");
        done.done();
    });

    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, SocketPairVectorReadWriteRoundTrip) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    WaitGroup done(1);
    go([&done, pair]() {
        char left[] = "ab";
        char right[] = "cd";
        struct iovec write_iov[2];
        write_iov[0].iov_base = left;
        write_iov[0].iov_len = 2;
        write_iov[1].iov_base = right;
        write_iov[1].iov_len = 2;

        EXPECT_EQ(co_writev(pair[0], write_iov, 2, 200), 4);

        char out_left[3] = {0};
        char out_right[3] = {0};
        struct iovec read_iov[2];
        read_iov[0].iov_base = out_left;
        read_iov[0].iov_len = 2;
        read_iov[1].iov_base = out_right;
        read_iov[1].iov_len = 2;

        EXPECT_EQ(co_readv(pair[1], read_iov, 2, 200), 4);
        EXPECT_STREQ(out_left, "ab");
        EXPECT_STREQ(out_right, "cd");
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, SocketPairSendRecvRoundTrip) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    WaitGroup done(1);
    go([&done, pair]() {
        const char *message = "pong";
        EXPECT_EQ(co_send(pair[0], message, 4, 0, 200), 4);

        char out[8] = {0};
        EXPECT_EQ(co_recv(pair[1], out, 4, 0, 200), 4);
        EXPECT_STREQ(out, "pong");
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, SocketApiHelpersExposeCoostStyleContracts) {
    int fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);

    const int before = co_error();
    co_error(EINTR);
    EXPECT_EQ(co_error(), EINTR);
    co_error(before);

    int reuse = 0;
    socklen_t reuse_len = sizeof(reuse);
    ASSERT_EQ(co_getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, &reuse_len),
              0);
    EXPECT_EQ(reuse, 0);

    co_set_reuseaddr(fd);
    reuse = 0;
    reuse_len = sizeof(reuse);
    ASSERT_EQ(co_getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, &reuse_len),
              0);
    EXPECT_EQ(reuse, 1);

    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookUnitByHeaderTest, SetSockoptTimeoutAffectsInfiniteTimeoutPath) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 20000;
    ASSERT_EQ(co_setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                            sizeof(timeout)),
              0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        char buffer[8] = {0};
        errno = 0;
        EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), kInfiniteTimeoutMs), -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, ShutdownRejectsInvalidHowFlag) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    errno = 0;
    EXPECT_EQ(co_shutdown(pair[0], 'x'), -1);
    EXPECT_EQ(errno, EINVAL);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, CloseWithDelayInCoroutinePathWorks) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        EXPECT_EQ(co_close(fd, 5), 0);
        done.done();
    });
    done.wait();

    ::close(pair[1]);
}

} // namespace
} // namespace zco
