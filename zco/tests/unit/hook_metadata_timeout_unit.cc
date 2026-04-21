#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zco {
namespace {

class HookMetadataTimeoutUnitTest : public test::RuntimeTestBase {};

int send_nosignal_flags_for_unit() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

TEST_F(HookMetadataTimeoutUnitTest, InvalidFdReadWriteFailWithEbadf) {
    init(1);

    WaitGroup done(1);
    go([&done]() {
        char buffer[8] = {0};

        errno = 0;
        EXPECT_EQ(co_read(-1, buffer, sizeof(buffer), 10), -1);
        EXPECT_EQ(errno, EBADF);

        errno = 0;
        EXPECT_EQ(co_write(-1, "x", 1, 10), -1);
        EXPECT_EQ(errno, EBADF);

        done.done();
    });
    done.wait();
}

TEST_F(HookMetadataTimeoutUnitTest, ExplicitTimeoutOverridesSocketTimeout) {
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
    timeout.tv_usec = 250000;
    ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)),
              0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        char buffer[8] = {0};
        const auto begin = std::chrono::steady_clock::now();
        errno = 0;
        EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), 10), -1);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();

        EXPECT_LT(elapsed_ms, 160);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, InfiniteTimeoutFallsBackToSocketTimeout) {
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
    timeout.tv_usec = 30000;
    ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)),
              0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        char buffer[8] = {0};
        const auto begin = std::chrono::steady_clock::now();
        errno = 0;
        EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), kInfiniteTimeoutMs), -1);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();

        EXPECT_LT(elapsed_ms, 400);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, DupMetadataSyncAndClosePathWorks) {
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
    timeout.tv_usec = 30000;
    ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)),
              0);

    int duplicated = ::dup(pair[0]);
    ASSERT_GE(duplicated, 0);
    sync_fd_metadata_on_dup(pair[0], duplicated);

    WaitGroup done(1);
    go([&done, fd = duplicated]() {
        char buffer[8] = {0};
        errno = 0;
        EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), kInfiniteTimeoutMs), -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();
    EXPECT_EQ(co_close(duplicated), 0);

    int api_dup = co_dup(pair[0]);
    ASSERT_GE(api_dup, 0);

    int dup2_target = ::dup(pair[1]);
    ASSERT_GE(dup2_target, 0);
    ASSERT_EQ(co_dup2(api_dup, dup2_target), dup2_target);

#if defined(__linux__)
    int dup3_target = ::dup(pair[1]);
    ASSERT_GE(dup3_target, 0);
    ASSERT_EQ(co_dup3(api_dup, dup3_target, O_CLOEXEC), dup3_target);
    EXPECT_EQ(co_close(dup3_target), 0);
#endif

    sync_fd_metadata_on_close(api_dup);
    EXPECT_EQ(co_close(api_dup), 0);
    EXPECT_EQ(co_close(dup2_target), 0);

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       ConcurrentReadWriteAcrossCoroutinesSucceeds) {
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
        const char *payload = "ping";
        EXPECT_EQ(co_write(pair[0], payload, 4, 100), 4);
        done.done();
    });

    go([&done, pair]() {
        char out[8] = {0};
        EXPECT_EQ(co_read(pair[1], out, 4, 100), 4);
        EXPECT_STREQ(out, "ping");
        done.done();
    });

    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       UserNonblockingReadTimesOutInCoroutineContext) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    const int flags = ::fcntl(pair[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(pair[0], F_SETFL, flags | O_NONBLOCK), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        char buffer[8] = {0};
        errno = 0;
        EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), 30), -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       Accept4OnUserNonblockingSocketTimesOutInCoroutineContext) {
    init(1);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);

    const int flags = ::fcntl(listen_fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK), 0);

    sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;

    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);

    WaitGroup done(1);
    go([&done, listen_fd, &listen_addr]() {
        errno = 0;
        socklen_t addr_len = sizeof(listen_addr);
        EXPECT_EQ(co_accept4(listen_fd,
                             reinterpret_cast<sockaddr *>(&listen_addr),
                             &addr_len, SOCK_CLOEXEC, 40),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnAggregatesFragmentsUntilFullLength) {
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

    go([&done, fd = pair[0]]() {
        EXPECT_EQ(co_write(fd, "ab", 2, 100), 2);
        yield();
        EXPECT_EQ(co_write(fd, "cdef", 4, 100), 4);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        char out[7] = {0};
        EXPECT_EQ(co_recvn(fd, out, 6, 0, 200), 6);
        EXPECT_STREQ(out, "abcdef");
        done.done();
    });

    done.wait();
    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       RecvnReturnsZeroWhenPeerClosesBeforeRequestedLength) {
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

    go([&done, fd = pair[0]]() {
        EXPECT_EQ(co_write(fd, "ab", 2, 100), 2);
        EXPECT_EQ(co_close(fd), 0);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        char out[5] = {0};
        EXPECT_EQ(co_recvn(fd, out, 4, 0, 200), 0);
        EXPECT_EQ(out[0], 'a');
        EXPECT_EQ(out[1], 'b');
        done.done();
    });

    done.wait();
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       SendtoNullAddressFallsBackToConnectedSendPath) {
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

    go([&done, fd = pair[0]]() {
        EXPECT_EQ(co_sendto(fd, "pong", 4, 0, nullptr, 0, 100), 4);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        char out[8] = {0};
        EXPECT_EQ(co_recv(fd, out, 4, 0, 100), 4);
        EXPECT_STREQ(out, "pong");
        done.done();
    });

    done.wait();
    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       RecvnAndSendWithZeroCountReturnZeroImmediately) {
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
    go([&done, fd0 = pair[0], fd1 = pair[1]]() {
        char out = 0;
        EXPECT_EQ(co_recvn(fd1, &out, 0, 0, 100), 0);
        EXPECT_EQ(co_send(fd0, "x", 0, 0, 100), 0);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, SendtoExplicitAddressZeroCountReturnsZero) {
    init(1);

    int recv_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(recv_fd, 0);
    int send_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(send_fd, 0);

    sockaddr_in recv_addr;
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0;

    ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                     sizeof(recv_addr)),
              0);
    socklen_t recv_addr_len = sizeof(recv_addr);
    ASSERT_EQ(::getsockname(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                            &recv_addr_len),
              0);

    WaitGroup done(1);
    go([&done, fd = send_fd, recv_addr]() {
        EXPECT_EQ(co_sendto(fd, "payload", 0, 0,
                            reinterpret_cast<const sockaddr *>(&recv_addr),
                            static_cast<socklen_t>(sizeof(recv_addr)), 100),
                  0);
        done.done();
    });
    done.wait();

    co_close(send_fd);
    co_close(recv_fd);
}

TEST_F(HookMetadataTimeoutUnitTest,
       ConnectOnAlreadyConnectedSocketReturnsSuccess) {
    init(2);

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int enable = 1;
    ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                           sizeof(enable)),
              0);

    sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;

    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);
    socklen_t listen_len = sizeof(listen_addr);
    ASSERT_EQ(::getsockname(listen_fd,
                            reinterpret_cast<sockaddr *>(&listen_addr),
                            &listen_len),
              0);

    WaitGroup done(2);

    go([&done, listen_fd]() {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        const int conn_fd =
            co_accept4(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                       &peer_len, SOCK_CLOEXEC, 1000);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd >= 0) {
            EXPECT_EQ(co_close(conn_fd), 0);
        }
        done.done();
    });

    go([&done, listen_addr]() {
        const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(co_connect(fd,
                             reinterpret_cast<const sockaddr *>(&listen_addr),
                             sizeof(listen_addr), 1000),
                  0);
        EXPECT_EQ(co_connect(fd,
                             reinterpret_cast<const sockaddr *>(&listen_addr),
                             sizeof(listen_addr), 1000),
                  0);
        EXPECT_EQ(co_close(fd), 0);
        done.done();
    });

    done.wait();
    co_close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvfromTimesOutWhenNoDatagramArrives) {
    init(1);

    int recv_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(recv_fd, 0);

    sockaddr_in recv_addr;
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0;
    ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                     sizeof(recv_addr)),
              0);

    WaitGroup done(1);
    go([&done, fd = recv_fd]() {
        char out[8] = {0};
        sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        errno = 0;
        EXPECT_EQ(co_recvfrom(fd, out, sizeof(out), 0,
                              reinterpret_cast<sockaddr *>(&peer), &peer_len,
                              30),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(recv_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, DupAndCloseInvalidFdReturnErrors) {
    init(1);

    errno = 0;
    EXPECT_EQ(co_dup(-1), -1);
    EXPECT_EQ(errno, EBADF);

    errno = 0;
    EXPECT_EQ(co_dup2(-1, 0), -1);
    EXPECT_EQ(errno, EBADF);

    errno = 0;
    EXPECT_EQ(co_close(-1), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(HookMetadataTimeoutUnitTest, InvalidFdInCoroutineReturnsEbadfForHooks) {
    init(1);

    WaitGroup done(1);
    go([&done]() {
        errno = 0;
        EXPECT_EQ(co_connect(-1, nullptr, 0, 10), -1);
        EXPECT_EQ(errno, EBADF);

        socklen_t addr_len = 0;
        errno = 0;
        EXPECT_EQ(co_accept(-1, nullptr, &addr_len, 10), -1);
        EXPECT_EQ(errno, EBADF);

        errno = 0;
        EXPECT_EQ(co_accept4(-1, nullptr, &addr_len, 0, 10), -1);
        EXPECT_EQ(errno, EBADF);
        done.done();
    });
    done.wait();
}

TEST_F(HookMetadataTimeoutUnitTest,
       ConnectWithMismatchedAddressFamilyFailsFast) {
    init(1);

    const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    WaitGroup done(1);
    go([&done, fd]() {
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, "/tmp/zco-unused.sock",
                     sizeof(addr.sun_path) - 1);

        errno = 0;
        EXPECT_EQ(co_connect(fd, reinterpret_cast<const sockaddr *>(&addr),
                             static_cast<socklen_t>(sizeof(addr)), 30),
                  -1);
        EXPECT_TRUE(errno == EAFNOSUPPORT || errno == EINVAL ||
                    errno == ENOTSOCK);
        done.done();
    });
    done.wait();

    co_close(fd);
}

TEST_F(HookMetadataTimeoutUnitTest,
       CoSendOnDisconnectedPeerReturnsPipeLikeError) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    ASSERT_EQ(co_close(pair[1]), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        errno = 0;
        EXPECT_EQ(co_send(fd, "x", 1, send_nosignal_flags_for_unit(), 50), -1);
        EXPECT_TRUE(errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
}

TEST_F(HookMetadataTimeoutUnitTest, SendtoWithInvalidAddressLengthFailsFast) {
    init(1);

    const int fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(9);

    WaitGroup done(1);
    go([&done, fd, addr]() {
        errno = 0;
        EXPECT_EQ(co_sendto(fd, "x", 1, 0,
                            reinterpret_cast<const sockaddr *>(&addr),
                            static_cast<socklen_t>(1), 50),
                  -1);
        EXPECT_TRUE(errno == EINVAL || errno == EFAULT ||
                    errno == EDESTADDRREQ || errno == ENOTSOCK);
        done.done();
    });
    done.wait();

    co_close(fd);
}

TEST_F(HookMetadataTimeoutUnitTest,
       ReadvAndWritevTransferFragmentsAcrossSocketPair) {
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
    go([&done, fd = pair[0]]() {
        char left[] = "ab";
        char right[] = "cd";
        iovec iov[2];
        iov[0].iov_base = left;
        iov[0].iov_len = 2;
        iov[1].iov_base = right;
        iov[1].iov_len = 2;
        EXPECT_EQ(co_writev(fd, iov, 2, 100), 4);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        char left[3] = {0};
        char right[3] = {0};
        iovec iov[2];
        iov[0].iov_base = left;
        iov[0].iov_len = 2;
        iov[1].iov_base = right;
        iov[1].iov_len = 2;
        EXPECT_EQ(co_readv(fd, iov, 2, 100), 4);
        EXPECT_STREQ(left, "ab");
        EXPECT_STREQ(right, "cd");
        done.done();
    });

    done.wait();
    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvfromReturnsPeerAddressAndPayload) {
    init(2);

    int recv_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(recv_fd, 0);
    int send_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(send_fd, 0);

    sockaddr_in recv_addr;
    std::memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0;
    ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                     sizeof(recv_addr)),
              0);
    socklen_t recv_addr_len = sizeof(recv_addr);
    ASSERT_EQ(::getsockname(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                            &recv_addr_len),
              0);

    WaitGroup done(2);
    go([&done, fd = send_fd, recv_addr]() {
        EXPECT_EQ(co_sendto(fd, "udp", 3, 0,
                            reinterpret_cast<const sockaddr *>(&recv_addr),
                            static_cast<socklen_t>(sizeof(recv_addr)), 100),
                  3);
        done.done();
    });

    go([&done, fd = recv_fd]() {
        char out[4] = {0};
        sockaddr_storage peer;
        std::memset(&peer, 0, sizeof(peer));
        socklen_t peer_len = sizeof(peer);
        EXPECT_EQ(co_recvfrom(fd, out, 3, 0,
                              reinterpret_cast<sockaddr *>(&peer), &peer_len,
                              100),
                  3);
        EXPECT_STREQ(out, "udp");
        EXPECT_EQ(peer.ss_family, AF_INET);
        EXPECT_GT(peer_len, 0);
        done.done();
    });

    done.wait();
    co_close(send_fd);
    co_close(recv_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, AcceptTimesOutWhenNoClientArrives) {
    init(1);

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    co_set_reuseaddr(listen_fd);

    sockaddr_in listen_addr;
    std::memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);

    WaitGroup done(1);
    go([&done, listen_fd]() {
        sockaddr_storage addr;
        std::memset(&addr, 0, sizeof(addr));
        socklen_t addr_len = sizeof(addr);
        errno = 0;
        EXPECT_EQ(co_accept(listen_fd, reinterpret_cast<sockaddr *>(&addr),
                            &addr_len, 30),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, AcceptReturnsNonblockingClientFd) {
    init(2);

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    co_set_reuseaddr(listen_fd);

    sockaddr_in listen_addr;
    std::memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);
    socklen_t listen_len = sizeof(listen_addr);
    ASSERT_EQ(::getsockname(listen_fd,
                            reinterpret_cast<sockaddr *>(&listen_addr),
                            &listen_len),
              0);

    WaitGroup done(2);
    go([&done, listen_fd]() {
        sockaddr_storage peer_addr;
        std::memset(&peer_addr, 0, sizeof(peer_addr));
        socklen_t peer_len = sizeof(peer_addr);
        const int accepted_fd =
            co_accept(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                      &peer_len, 1000);
        EXPECT_GE(accepted_fd, 0);
        if (accepted_fd >= 0) {
            const int flags = ::fcntl(accepted_fd, F_GETFL, 0);
            EXPECT_GE(flags, 0);
            EXPECT_NE(flags & O_NONBLOCK, 0);
            EXPECT_EQ(co_close(accepted_fd), 0);
        }
        done.done();
    });

    go([&done, listen_addr]() {
        const int client_fd = co_socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client_fd, 0);
        ASSERT_EQ(co_connect(client_fd,
                             reinterpret_cast<const sockaddr *>(&listen_addr),
                             sizeof(listen_addr), 1000),
                  0);
        EXPECT_EQ(co_close(client_fd), 0);
        done.done();
    });

    done.wait();
    co_close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest,
       SendtoNullAddressWithNonzeroLengthFailsFast) {
    init(1);

    const int fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);

    WaitGroup done(1);
    go([&done, fd]() {
        errno = 0;
        EXPECT_EQ(co_sendto(fd, "x", 1, 0, nullptr, 1, 30), -1);
        EXPECT_TRUE(errno == EDESTADDRREQ || errno == EINVAL ||
                    errno == EFAULT || errno == ENOTCONN);
        done.done();
    });
    done.wait();

    co_close(fd);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnZeroCountReturnsImmediately) {
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
    go([&done, fd = pair[0]]() {
        char sink = 0;
        EXPECT_EQ(co_recvn(fd, &sink, 0, 0, 20), 0);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       RecvnReturnsZeroWhenPeerClosesBeforeFullRead) {
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
    go([&done, fd = pair[0]]() {
        char out[8] = {0};
        EXPECT_EQ(co_recvn(fd, out, sizeof(out), 0, 100), 0);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        const char payload[3] = {'a', 'b', 'c'};
        EXPECT_EQ(co_send(fd, payload, sizeof(payload),
                          send_nosignal_flags_for_unit(), 50),
                  3);
        co_close(fd);
        done.done();
    });

    done.wait();
    co_close(pair[0]);
}

TEST_F(HookMetadataTimeoutUnitTest, CoSendTimesOutWhenPeerNeverReads) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    const int small = 1024;
    ASSERT_EQ(
        ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        std::string payload(4 * 1024 * 1024, 'x');
        errno = 0;
        EXPECT_EQ(co_send(fd, payload.data(), payload.size(),
                          send_nosignal_flags_for_unit(), 20),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, InvalidFdForRecvnSendAndSendtoReturnEbadf) {
    init(1);

    WaitGroup done(1);
    go([&done]() {
        char buffer[4] = {0};
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(9);

        errno = 0;
        EXPECT_EQ(co_recvn(-1, buffer, sizeof(buffer), 0, 10), -1);
        EXPECT_EQ(errno, EBADF);

        errno = 0;
        EXPECT_EQ(co_send(-1, "x", 1, send_nosignal_flags_for_unit(), 10), -1);
        EXPECT_EQ(errno, EBADF);

        errno = 0;
        EXPECT_EQ(co_sendto(-1, "x", 1, 0,
                            reinterpret_cast<const sockaddr *>(&addr),
                            static_cast<socklen_t>(sizeof(addr)), 10),
                  -1);
        EXPECT_EQ(errno, EBADF);

        done.done();
    });
    done.wait();
}

TEST_F(HookMetadataTimeoutUnitTest,
       SendtoNullAddressAndZeroLengthFallsBackToConnectedSendPath) {
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
    go([&done, fd = pair[0]]() {
        EXPECT_EQ(co_sendto(fd, "xy", 2, send_nosignal_flags_for_unit(),
                            nullptr, 0, 100),
                  2);
        done.done();
    });

    go([&done, fd = pair[1]]() {
        char out[3] = {0};
        EXPECT_EQ(co_recv(fd, out, 2, 0, 100), 2);
        EXPECT_STREQ(out, "xy");
        done.done();
    });

    done.wait();
    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       BlockingDescriptorsAreRejectedInsideCoroutineWithEinval) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    const int left_flags = ::fcntl(pair[0], F_GETFL, 0);
    const int right_flags = ::fcntl(pair[1], F_GETFL, 0);
    ASSERT_GE(left_flags, 0);
    ASSERT_GE(right_flags, 0);
    ASSERT_EQ(::fcntl(pair[0], F_SETFL, left_flags & ~O_NONBLOCK), 0);
    ASSERT_EQ(::fcntl(pair[1], F_SETFL, right_flags & ~O_NONBLOCK), 0);

    WaitGroup done(1);
    go([&done, left = pair[0], right = pair[1]]() {
        char buffer[2] = {0};
        errno = 0;
        EXPECT_EQ(co_read(left, buffer, sizeof(buffer), 10), -1);
        EXPECT_EQ(errno, EINVAL);

        errno = 0;
        EXPECT_EQ(co_send(right, "x", 1, send_nosignal_flags_for_unit(), 10),
                  -1);
        EXPECT_EQ(errno, EINVAL);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, SyncMetadataOnDupWithClosedTargetIsSafe) {
    const int fd = co_tcp_socket(AF_INET);
    ASSERT_GE(fd, 0);

    const int duplicated = ::dup(fd);
    ASSERT_GE(duplicated, 0);
    ASSERT_EQ(co_close(duplicated), 0);

    sync_fd_metadata_on_dup(-1, duplicated);
    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnWithInvalidFlagsReturnsEinval) {
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
    go([&done, fd = pair[0]]() {
        char out[4] = {0};
        errno = 0;
        EXPECT_EQ(co_recvn(fd, out, 1, -1, 30), -1);
        EXPECT_EQ(errno, EINVAL);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnWithZeroTimeoutReturnsEtimedout) {
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
    go([&done, fd = pair[1]]() {
        char out[4] = {0};
        errno = 0;
        EXPECT_EQ(co_recvn(fd, out, 1, 0, 0), -1);
        EXPECT_EQ(errno, ETIMEDOUT);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       CoSendWithZeroTimeoutFailsWhenWriteWouldBlock) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(
        ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK),
        0);
    ASSERT_EQ(
        ::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK),
        0);

    const int small = 1024;
    ASSERT_EQ(
        ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        std::string payload(4 * 1024 * 1024, 'x');
        errno = 0;
        EXPECT_EQ(co_send(fd, payload.data(), payload.size(),
                          send_nosignal_flags_for_unit(), 0),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    co_close(pair[0]);
    co_close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest,
       ConnectAndAcceptWithZeroTimeoutCoverImmediateTimeoutPaths) {
    init(2);

    const int listen_fd = co_tcp_socket(AF_INET);
    ASSERT_GE(listen_fd, 0);
    co_set_reuseaddr(listen_fd);

    sockaddr_in listen_addr;
    std::memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 8), 0);
    socklen_t listen_len = sizeof(listen_addr);
    ASSERT_EQ(::getsockname(listen_fd,
                            reinterpret_cast<sockaddr *>(&listen_addr),
                            &listen_len),
              0);

    WaitGroup done(2);
    go([&done, listen_fd]() {
        sockaddr_storage peer;
        std::memset(&peer, 0, sizeof(peer));
        socklen_t peer_len = sizeof(peer);
        errno = 0;
        EXPECT_EQ(co_accept(listen_fd, reinterpret_cast<sockaddr *>(&peer),
                            &peer_len, 0),
                  -1);
        EXPECT_EQ(errno, ETIMEDOUT);
        done.done();
    });

    go([&done, listen_addr]() {
        const int client_fd = co_tcp_socket(AF_INET);
        ASSERT_GE(client_fd, 0);
        errno = 0;
        EXPECT_EQ(co_connect(client_fd,
                             reinterpret_cast<const sockaddr *>(&listen_addr),
                             static_cast<socklen_t>(sizeof(listen_addr)), 0),
                  -1);
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EINPROGRESS ||
                    errno == EALREADY || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        EXPECT_EQ(co_close(client_fd), 0);
        done.done();
    });

    done.wait();
    co_close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, AcceptOnNonListeningSocketReturnsError) {
    init(1);

    const int fd = co_tcp_socket(AF_INET);
    ASSERT_GE(fd, 0);

    WaitGroup done(1);
    go([&done, fd]() {
        sockaddr_storage peer;
        std::memset(&peer, 0, sizeof(peer));
        socklen_t peer_len = sizeof(peer);
        errno = 0;
        EXPECT_EQ(
            co_accept(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len, 50),
            -1);
        EXPECT_TRUE(errno == EINVAL || errno == ENOTSOCK || errno == EBADF ||
                    errno == EOPNOTSUPP);
        done.done();
    });
    done.wait();

    co_close(fd);
}

#if defined(__linux__)
TEST_F(HookMetadataTimeoutUnitTest, Accept4TimeoutAndErrorPathsAreCovered) {
    init(1);

    const int listen_fd = co_tcp_socket(AF_INET);
    ASSERT_GE(listen_fd, 0);
    co_set_reuseaddr(listen_fd);

    sockaddr_in listen_addr;
    std::memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);

    WaitGroup timeout_done(1);
    go([&timeout_done, listen_fd]() {
        sockaddr_storage peer;
        std::memset(&peer, 0, sizeof(peer));
        socklen_t peer_len = sizeof(peer);
        errno = 0;
        EXPECT_EQ(co_accept4(listen_fd, reinterpret_cast<sockaddr *>(&peer),
                             &peer_len, 0, 0),
                  -1);
        EXPECT_EQ(errno, ETIMEDOUT);
        timeout_done.done();
    });
    timeout_done.wait();
    co_close(listen_fd);

    const int plain_fd = co_tcp_socket(AF_INET);
    ASSERT_GE(plain_fd, 0);
    WaitGroup invalid_done(1);
    go([&invalid_done, plain_fd]() {
        sockaddr_storage peer;
        std::memset(&peer, 0, sizeof(peer));
        socklen_t peer_len = sizeof(peer);
        errno = 0;
        EXPECT_EQ(co_accept4(plain_fd, reinterpret_cast<sockaddr *>(&peer),
                             &peer_len, 0, 50),
                  -1);
        EXPECT_TRUE(errno == EINVAL || errno == ENOTSOCK || errno == EBADF ||
                    errno == EOPNOTSUPP);
        invalid_done.done();
    });
    invalid_done.wait();
    co_close(plain_fd);
}
#endif

} // namespace
} // namespace zco
