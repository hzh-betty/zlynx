#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class HookMetadataTimeoutUnitTest : public test::RuntimeTestBase {};

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
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 250000;
  ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

  WaitGroup done(1);
  go([&done, fd = pair[0]]() {
    char buffer[8] = {0};
    const auto begin = std::chrono::steady_clock::now();
    errno = 0;
    EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), 10), -1);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin)
                                .count();

    EXPECT_LT(elapsed_ms, 160);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
    done.done();
  });
  done.wait();

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, InfiniteTimeoutFallsBackToSocketTimeout) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 30000;
  ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

  WaitGroup done(1);
  go([&done, fd = pair[0]]() {
    char buffer[8] = {0};
    const auto begin = std::chrono::steady_clock::now();
    errno = 0;
    EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), kInfiniteTimeoutMs), -1);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin)
                                .count();

    EXPECT_LT(elapsed_ms, 400);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
    done.done();
  });
  done.wait();

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, DupMetadataSyncAndClosePathWorks) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 30000;
  ASSERT_EQ(::setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

  int duplicated = ::dup(pair[0]);
  ASSERT_GE(duplicated, 0);
  sync_fd_metadata_on_dup(pair[0], duplicated);

  WaitGroup done(1);
  go([&done, fd = duplicated]() {
    char buffer[8] = {0};
    errno = 0;
    EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), kInfiniteTimeoutMs), -1);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
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

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, ConcurrentReadWriteAcrossCoroutinesSucceeds) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  WaitGroup done(2);

  go([&done, pair]() {
    const char* payload = "ping";
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

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, UserNonblockingReadTimesOutInCoroutineContext) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  const int flags = ::fcntl(pair[0], F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, flags | O_NONBLOCK), 0);

  WaitGroup done(1);
  go([&done, fd = pair[0]]() {
    char buffer[8] = {0};
    errno = 0;
    EXPECT_EQ(co_read(fd, buffer, sizeof(buffer), 30), -1);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
    done.done();
  });
  done.wait();

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, Accept4OnUserNonblockingSocketTimesOutInCoroutineContext) {
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

  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 4), 0);

  WaitGroup done(1);
  go([&done, listen_fd, &listen_addr]() {
    errno = 0;
    socklen_t addr_len = sizeof(listen_addr);
    EXPECT_EQ(co_accept4(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), &addr_len,
                         SOCK_CLOEXEC, 40),
              -1);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
    done.done();
  });
  done.wait();

  ::close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnAggregatesFragmentsUntilFullLength) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

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
  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnReturnsZeroWhenPeerClosesBeforeRequestedLength) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

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
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, SendtoNullAddressFallsBackToConnectedSendPath) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

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
  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvnAndSendWithZeroCountReturnZeroImmediately) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  ASSERT_EQ(::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK), 0);
  ASSERT_EQ(::fcntl(pair[1], F_SETFL, ::fcntl(pair[1], F_GETFL, 0) | O_NONBLOCK), 0);

  WaitGroup done(1);
  go([&done, fd0 = pair[0], fd1 = pair[1]]() {
    char out = 0;
    EXPECT_EQ(co_recvn(fd1, &out, 0, 0, 100), 0);
    EXPECT_EQ(co_send(fd0, "x", 0, 0, 100), 0);
    done.done();
  });
  done.wait();

  ::close(pair[0]);
  ::close(pair[1]);
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

  ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr*>(&recv_addr), sizeof(recv_addr)), 0);
  socklen_t recv_addr_len = sizeof(recv_addr);
  ASSERT_EQ(::getsockname(recv_fd, reinterpret_cast<sockaddr*>(&recv_addr), &recv_addr_len), 0);

  WaitGroup done(1);
  go([&done, fd = send_fd, recv_addr]() {
    EXPECT_EQ(co_sendto(fd, "payload", 0, 0, reinterpret_cast<const sockaddr*>(&recv_addr),
                        static_cast<socklen_t>(sizeof(recv_addr)), 100),
              0);
    done.done();
  });
  done.wait();

  ::close(send_fd);
  ::close(recv_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, ConnectOnAlreadyConnectedSocketReturnsSuccess) {
  init(2);

  int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);
  int enable = 1;
  ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)), 0);

  sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listen_addr.sin_port = 0;

  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 4), 0);
  socklen_t listen_len = sizeof(listen_addr);
  ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), &listen_len), 0);

  WaitGroup done(2);

  go([&done, listen_fd]() {
    sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    const int conn_fd =
        co_accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len, SOCK_CLOEXEC, 1000);
    EXPECT_GE(conn_fd, 0);
    if (conn_fd >= 0) {
      EXPECT_EQ(co_close(conn_fd), 0);
    }
    done.done();
  });

  go([&done, listen_addr]() {
    const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(co_connect(fd, reinterpret_cast<const sockaddr*>(&listen_addr), sizeof(listen_addr), 1000),
              0);
    EXPECT_EQ(co_connect(fd, reinterpret_cast<const sockaddr*>(&listen_addr), sizeof(listen_addr), 1000),
              0);
    EXPECT_EQ(co_close(fd), 0);
    done.done();
  });

  done.wait();
  ::close(listen_fd);
}

TEST_F(HookMetadataTimeoutUnitTest, RecvfromTimesOutWhenNoDatagramArrives) {
  init(1);

  int recv_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(recv_fd, 0);

  sockaddr_in recv_addr;
  recv_addr.sin_family = AF_INET;
  recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  recv_addr.sin_port = 0;
  ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr*>(&recv_addr), sizeof(recv_addr)), 0);

  WaitGroup done(1);
  go([&done, fd = recv_fd]() {
    char out[8] = {0};
    sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    errno = 0;
    EXPECT_EQ(co_recvfrom(fd, out, sizeof(out), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len,
                          30),
              -1);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK);
    done.done();
  });
  done.wait();

  ::close(recv_fd);
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

}  // namespace
}  // namespace zcoroutine
