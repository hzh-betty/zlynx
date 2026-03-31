#include <atomic>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "zcoroutine/io_event.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class IoEventUnitTest : public test::RuntimeTestBase {};

TEST_F(IoEventUnitTest, InvalidFdReturnsFalseAndEbadf) {
  IoEvent event(-1, IoEventType::kRead);

  errno = 0;
  EXPECT_FALSE(event.wait(1));
  EXPECT_EQ(errno, EBADF);
}

TEST_F(IoEventUnitTest, InvalidEventTypeReturnsFalseAndEinval) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  IoEvent event(pair[0], static_cast<IoEventType>(0x999));
  errno = 0;
  EXPECT_FALSE(event.wait(1));
  EXPECT_EQ(errno, EINVAL);

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(IoEventUnitTest, ReadReadyReturnsTrue) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  const char marker = 'z';
  ASSERT_EQ(::write(pair[0], &marker, 1), 1);

  IoEvent event(pair[1], IoEventType::kRead);
  EXPECT_TRUE(event.wait(20));

  char out = 0;
  EXPECT_EQ(::read(pair[1], &out, 1), 1);
  EXPECT_EQ(out, marker);

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(IoEventUnitTest, ReadTimeoutReturnsFalse) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  IoEvent event(pair[1], IoEventType::kRead);
  errno = 0;
  EXPECT_FALSE(event.wait(5));

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(IoEventUnitTest, WriteReadyReturnsTrue) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  IoEvent event(pair[0], IoEventType::kWrite);
  EXPECT_TRUE(event.wait(10));

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(IoEventUnitTest, CoroutineTimeoutKeepsTimeoutStateAndPropagatesErrno) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  WaitGroup done(1);
  std::atomic<bool> timed_out(false);
  std::atomic<int> captured_errno(0);

  go([&done, &timed_out, &captured_errno, fd = pair[1]]() {
    IoEvent event(fd, IoEventType::kRead);
    errno = 0;
    const bool ok = event.wait(8);
    timed_out.store(!ok && timeout(), std::memory_order_release);
    captured_errno.store(errno, std::memory_order_release);
    done.done();
  });

  done.wait();
  EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
  const int err = captured_errno.load(std::memory_order_acquire);
  EXPECT_TRUE(err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK);

  ::close(pair[0]);
  ::close(pair[1]);
}

}  // namespace
}  // namespace zcoroutine
