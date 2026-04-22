#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/hook.h"

namespace zco {
namespace {

class HookHelperUnitTest : public test::RuntimeTestBase {};

TEST_F(HookHelperUnitTest, CloseDelayIgnoredOutsideCoroutine) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(co_close(pair[0], 80), 0);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin)
            .count();

    EXPECT_LT(elapsed_ms, 40);
    EXPECT_EQ(co_close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, CloseDelayWorksInsideCoroutine) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    std::atomic<long long> elapsed_ms(0);

    go([&done, &elapsed_ms, fd = pair[0]]() {
        const auto begin = std::chrono::steady_clock::now();
        EXPECT_EQ(co_close(fd, 20), 0);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        elapsed_ms.store(elapsed, std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 15);

    EXPECT_EQ(co_close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ShutdownInvalidModeReturnsEinval) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    errno = 0;
    EXPECT_EQ(co_shutdown(pair[0], 'x'), -1);
    EXPECT_EQ(errno, EINVAL);

    EXPECT_EQ(co_close(pair[0]), 0);
    EXPECT_EQ(co_close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ShutdownWriteKeepsReverseReadPathAlive) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    EXPECT_EQ(co_shutdown(pair[0], 'w'), 0);

    char marker = 'z';
#if defined(MSG_NOSIGNAL)
    const ssize_t write_after_shutdown =
        ::send(pair[0], &marker, 1, MSG_NOSIGNAL);
#else
    const ssize_t write_after_shutdown = ::send(pair[0], &marker, 1, 0);
#endif
    EXPECT_EQ(write_after_shutdown, -1);
    EXPECT_TRUE(errno == EPIPE || errno == ESHUTDOWN);

    char eof_probe = 0;
    EXPECT_EQ(::recv(pair[1], &eof_probe, 1, 0), 0);

#if defined(MSG_NOSIGNAL)
    ASSERT_EQ(::send(pair[1], &marker, 1, MSG_NOSIGNAL), 1);
#else
    ASSERT_EQ(::send(pair[1], &marker, 1, 0), 1);
#endif
    char recv_back = 0;
    ASSERT_EQ(::recv(pair[0], &recv_back, 1, 0), 1);
    EXPECT_EQ(recv_back, marker);

    EXPECT_EQ(co_close(pair[0]), 0);
    EXPECT_EQ(co_close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ShutdownReadAndBothModesBehaveAsExpected) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    EXPECT_EQ(co_shutdown(pair[0], 'r'), 0);

    char marker = 'k';
#if defined(MSG_NOSIGNAL)
    const ssize_t write_after_read_shutdown =
        ::send(pair[1], &marker, 1, MSG_NOSIGNAL);
#else
    const ssize_t write_after_read_shutdown = ::send(pair[1], &marker, 1, 0);
#endif
    EXPECT_EQ(write_after_read_shutdown, -1);
    EXPECT_TRUE(errno == EPIPE || errno == ECONNRESET || errno == ESHUTDOWN ||
                errno == ENOTCONN);

    EXPECT_EQ(co_shutdown(pair[1], 'b'), 0);
#if defined(MSG_NOSIGNAL)
    const ssize_t write_after_both = ::send(pair[1], &marker, 1, MSG_NOSIGNAL);
#else
    const ssize_t write_after_both = ::send(pair[1], &marker, 1, 0);
#endif
    EXPECT_EQ(write_after_both, -1);
    EXPECT_TRUE(errno == EPIPE || errno == ESHUTDOWN || errno == ENOTCONN);

    EXPECT_EQ(co_close(pair[0]), 0);
    EXPECT_EQ(co_close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ResetTcpSocketSetsLingerAndClosesFd) {
    int fd = co_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_GE(fd, 0);

    const int dup_fd = ::dup(fd);
    ASSERT_GE(dup_fd, 0);

    EXPECT_EQ(co_reset_tcp_socket(fd, 0), 0);

    errno = 0;
    EXPECT_EQ(::fcntl(fd, F_GETFD, 0), -1);
    EXPECT_EQ(errno, EBADF);

    linger option;
    socklen_t option_len = sizeof(option);
    ASSERT_EQ(::getsockopt(dup_fd, SOL_SOCKET, SO_LINGER, &option, &option_len),
              0);
    EXPECT_EQ(option.l_onoff, 1);
    EXPECT_EQ(option.l_linger, 0);

    EXPECT_EQ(co_close(dup_fd), 0);
}

TEST_F(HookHelperUnitTest, SetCloexecEnablesFdCloexecFlag) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int flags = ::fcntl(fd, F_GETFD, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC), 0);

    co_set_cloexec(fd);

    flags = ::fcntl(fd, F_GETFD, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & FD_CLOEXEC, 0);

    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookHelperUnitTest, TcpAndUdpFactorySocketsAreNonblocking) {
    const int tcp_fd = co_tcp_socket(AF_INET);
    ASSERT_GE(tcp_fd, 0);
    const int udp_fd = co_udp_socket(AF_INET);
    ASSERT_GE(udp_fd, 0);

    int tcp_flags = ::fcntl(tcp_fd, F_GETFL, 0);
    int udp_flags = ::fcntl(udp_fd, F_GETFL, 0);
    ASSERT_GE(tcp_flags, 0);
    ASSERT_GE(udp_flags, 0);
    EXPECT_NE(tcp_flags & O_NONBLOCK, 0);
    EXPECT_NE(udp_flags & O_NONBLOCK, 0);

    EXPECT_EQ(co_close(tcp_fd), 0);
    EXPECT_EQ(co_close(udp_fd), 0);
}

TEST_F(HookHelperUnitTest, SocketOptionHelpersApplyExpectedOptions) {
    const int fd = co_tcp_socket(AF_INET);
    ASSERT_GE(fd, 0);

    co_set_nonblock(fd);
    int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);

    co_set_send_buffer_size(fd, 128 * 1024);
    int sndbuf = 0;
    socklen_t sndbuf_len = sizeof(sndbuf);
    ASSERT_EQ(::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sndbuf_len), 0);
    EXPECT_GT(sndbuf, 0);

    co_set_recv_buffer_size(fd, 128 * 1024);
    int rcvbuf = 0;
    socklen_t rcvbuf_len = sizeof(rcvbuf);
    ASSERT_EQ(::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rcvbuf_len), 0);
    EXPECT_GT(rcvbuf, 0);

    co_set_tcp_keepalive(fd);
    int keepalive = 0;
    socklen_t keepalive_len = sizeof(keepalive);
    ASSERT_EQ(
        ::getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &keepalive_len),
        0);
    EXPECT_EQ(keepalive, 1);

    co_set_tcp_nodelay(fd);
    int nodelay = 0;
    socklen_t nodelay_len = sizeof(nodelay);
    ASSERT_EQ(
        ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &nodelay_len), 0);
    EXPECT_EQ(nodelay, 1);

    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookHelperUnitTest, CoBindAndListenWrapSystemCalls) {
    const int fd = co_tcp_socket(AF_INET);
    ASSERT_GE(fd, 0);
    co_set_reuseaddr(fd);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    ASSERT_EQ(co_bind(fd, reinterpret_cast<const sockaddr *>(&addr),
                      static_cast<socklen_t>(sizeof(addr))),
              0);
    ASSERT_EQ(co_listen(fd, 8), 0);

    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookHelperUnitTest, MetadataSyncHelpersHandleInvalidInputsSafely) {
    sync_fd_metadata_on_close(-1);
    sync_fd_metadata_on_dup(-1, -1);
    SUCCEED();
}

TEST_F(HookHelperUnitTest, Dup3InvalidFdReturnsEbadfOnLinux) {
#if defined(__linux__)
    errno = 0;
    EXPECT_EQ(co_dup3(-1, 0, 0), -1);
    EXPECT_EQ(errno, EBADF);
#endif
}

TEST_F(HookHelperUnitTest,
       SyncMetadataOnDupWithoutSourceMetadataUsesTargetFallback) {
    const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    // Remove source metadata first, then sync from invalid source to valid
    // target to exercise fallback path.
    sync_fd_metadata_on_close(fd);
    const int dup_fd = ::dup(fd);
    ASSERT_GE(dup_fd, 0);
    sync_fd_metadata_on_dup(-1, dup_fd);

    EXPECT_EQ(co_close(dup_fd), 0);
    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookHelperUnitTest, ResetTcpSocketInvalidFdFails) {
    errno = 0;
    EXPECT_EQ(co_reset_tcp_socket(-1, 0), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(HookHelperUnitTest, InvalidSocketCreationReturnsError) {
    errno = 0;
    EXPECT_EQ(co_socket(-1, SOCK_STREAM, 0), -1);
    EXPECT_NE(errno, 0);
}

TEST_F(HookHelperUnitTest, SocketOptionWrappersOnInvalidFdReturnEbadf) {
    int value = 0;
    socklen_t value_len = sizeof(value);
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;

    errno = 0;
    EXPECT_EQ(co_getsockopt(-1, SOL_SOCKET, SO_RCVBUF, &value, &value_len), -1);
    EXPECT_EQ(errno, EBADF);

    errno = 0;
    EXPECT_EQ(co_setsockopt(-1, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                            static_cast<socklen_t>(sizeof(timeout))),
              -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(HookHelperUnitTest,
       MetadataCacheCanBeRebuiltAfterExplicitMetadataRemoval) {
    const int fd = co_tcp_socket(AF_INET);
    ASSERT_GE(fd, 0);

    sync_fd_metadata_on_close(fd);

    timeval recv_timeout;
    recv_timeout.tv_sec = 0;
    recv_timeout.tv_usec = 12000;
    ASSERT_EQ(co_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                            static_cast<socklen_t>(sizeof(recv_timeout))),
              0);

    timeval send_timeout;
    send_timeout.tv_sec = 0;
    send_timeout.tv_usec = 8000;
    ASSERT_EQ(co_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                            static_cast<socklen_t>(sizeof(send_timeout))),
              0);

    const int duplicated = ::dup(fd);
    ASSERT_GE(duplicated, 0);
    sync_fd_metadata_on_dup(-1, duplicated);

    EXPECT_EQ(co_close(duplicated), 0);
    EXPECT_EQ(co_close(fd), 0);
}

TEST_F(HookHelperUnitTest, SetNonblockOnInvalidFdDoesNotAbort) {
    co_set_nonblock(-1);
    SUCCEED();
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
