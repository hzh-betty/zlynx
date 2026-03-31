#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "zcoroutine/internal/runtime_manager.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class ProcessorWaitTimerUnitTest : public test::RuntimeTestBase {};

TEST_F(ProcessorWaitTimerUnitTest, OutsideCoroutineParkAndAddTimerReturnSafeDefaults) {
  EXPECT_FALSE(park_current());
  EXPECT_FALSE(park_current_for(1));

  std::shared_ptr<TimerToken> token = add_timer(1, []() {});
  EXPECT_EQ(token, nullptr);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdFallbackHandlesReadableAndTimeout) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  const char marker = 'x';
  ASSERT_EQ(::write(pair[0], &marker, 1), 1);
  EXPECT_TRUE(wait_fd(pair[1], EPOLLIN, 20));

  char out = 0;
  ASSERT_EQ(::read(pair[1], &out, 1), 1);
  EXPECT_FALSE(wait_fd(pair[1], EPOLLIN, 5));

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdWithNoMappedPollEventsReturnsFalse) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  errno = 0;
  EXPECT_FALSE(wait_fd(pair[1], 0, 5));

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdFallbackHandlesWritableEvent) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  EXPECT_TRUE(wait_fd(pair[0], EPOLLOUT, 10));

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdInvalidFdReturnsFalseWithoutChangingErrno) {
  errno = 0;
  EXPECT_FALSE(wait_fd(-1, EPOLLIN, 5));
  EXPECT_EQ(errno, 0);
}

}  // namespace
}  // namespace zcoroutine
