#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include <gtest/gtest.h>

#include "zcoroutine/hook.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class HookHelperUnitTest : public test::RuntimeTestBase {};

TEST_F(HookHelperUnitTest, CloseDelayIgnoredOutsideCoroutine) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  const auto begin = std::chrono::steady_clock::now();
  EXPECT_EQ(co_close(pair[0], 80), 0);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin)
                              .count();

  EXPECT_LT(elapsed_ms, 40);
  EXPECT_EQ(::close(pair[1]), 0);
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
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin)
                             .count();
    elapsed_ms.store(elapsed, std::memory_order_release);
    done.done();
  });

  done.wait();
  EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 15);

  EXPECT_EQ(::close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ShutdownInvalidModeReturnsEinval) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  errno = 0;
  EXPECT_EQ(co_shutdown(pair[0], 'x'), -1);
  EXPECT_EQ(errno, EINVAL);

  EXPECT_EQ(::close(pair[0]), 0);
  EXPECT_EQ(::close(pair[1]), 0);
}

TEST_F(HookHelperUnitTest, ShutdownWriteKeepsReverseReadPathAlive) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  EXPECT_EQ(co_shutdown(pair[0], 'w'), 0);

  char marker = 'z';
#if defined(MSG_NOSIGNAL)
  const ssize_t write_after_shutdown = ::send(pair[0], &marker, 1, MSG_NOSIGNAL);
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

  EXPECT_EQ(::close(pair[0]), 0);
  EXPECT_EQ(::close(pair[1]), 0);
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
  ASSERT_EQ(::getsockopt(dup_fd, SOL_SOCKET, SO_LINGER, &option, &option_len), 0);
  EXPECT_EQ(option.l_onoff, 1);
  EXPECT_EQ(option.l_linger, 0);

  EXPECT_EQ(::close(dup_fd), 0);
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

  EXPECT_EQ(::close(fd), 0);
}

}  // namespace
}  // namespace zcoroutine
