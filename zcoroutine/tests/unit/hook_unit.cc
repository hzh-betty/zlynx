#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "zcoroutine/hook.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class HookUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(HookUnitByHeaderTest, InvalidFdPathsReturnExpectedErrors) {
  char buffer[4] = {0};

  errno = 0;
  EXPECT_EQ(co_read(-1, buffer, sizeof(buffer), 10), -1);
  EXPECT_EQ(errno, EBADF);

  errno = 0;
  EXPECT_EQ(co_write(-1, "x", 1, 10), -1);
  EXPECT_EQ(errno, EBADF);

  errno = 0;
  EXPECT_EQ(co_close(-1), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST_F(HookUnitByHeaderTest, SocketPairReadWriteRoundTrip) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  WaitGroup done(2);

  go([&done, pair]() {
    const char* message = "ok";
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
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

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

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(HookUnitByHeaderTest, SocketPairSendRecvRoundTrip) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  const char* message = "pong";
  EXPECT_EQ(co_send(pair[0], message, 4, 0, 200), 4);

  char out[8] = {0};
  EXPECT_EQ(co_recv(pair[1], out, 4, 0, 200), 4);
  EXPECT_STREQ(out, "pong");

  ::close(pair[0]);
  ::close(pair[1]);
}

}  // namespace
}  // namespace zcoroutine
