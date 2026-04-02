#include <atomic>
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

TEST_F(ProcessorWaitTimerUnitTest, OutsideCoroutineWaitFdReturnsEperm) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  errno = 0;
  EXPECT_FALSE(wait_fd(pair[1], EPOLLIN, 5));
  EXPECT_EQ(errno, EPERM);

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdInCoroutineHandlesReadableAndTimeout) {
  init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  WaitGroup done(2);
  go([&done, fd = pair[1]]() {
    EXPECT_TRUE(wait_fd(fd, EPOLLIN, 200));

    char out = 0;
    ASSERT_EQ(::read(fd, &out, 1), 1);
    EXPECT_EQ(out, 'x');
    done.done();
  });

  go([&done, fd = pair[0]]() {
    co_sleep_for(10);
    const char marker = 'x';
    ASSERT_EQ(::write(fd, &marker, 1), 1);
    done.done();
  });

  done.wait();

  WaitGroup timeout_done(1);
  std::atomic<bool> timed_out(false);
  std::atomic<int> captured_errno(0);
  go([&timeout_done, &timed_out, &captured_errno, fd = pair[1]]() {
    errno = 0;
    EXPECT_FALSE(wait_fd(fd, EPOLLIN, 5));
    timed_out.store(timeout(), std::memory_order_release);
    captured_errno.store(errno, std::memory_order_release);
    timeout_done.done();
  });
  timeout_done.wait();

  EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
  const int err = captured_errno.load(std::memory_order_acquire);
  EXPECT_TRUE(err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK);

  ::close(pair[0]);
  ::close(pair[1]);
}

TEST_F(ProcessorWaitTimerUnitTest, WaitFdWithNoEventsInCoroutineReturnsEinval) {
  init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  WaitGroup done(1);
  go([&done, fd = pair[1]]() {
    errno = 0;
    EXPECT_FALSE(wait_fd(fd, 0, 5));
    EXPECT_EQ(errno, EINVAL);
    done.done();
  });
  done.wait();

  ::close(pair[0]);
  ::close(pair[1]);
}

}  // namespace
}  // namespace zcoroutine
