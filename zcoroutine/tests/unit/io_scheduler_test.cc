#include "io_scheduler.h"
#include "zcoroutine_logger.h"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace zcoroutine;

class IoSchedulerTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(IoSchedulerTest, AddInvalidFd) {
  IoScheduler scheduler(1, "test");

  // FD -1 is invalid
  EXPECT_EQ(scheduler.add_event(-1, Channel::kRead), -1);
}

TEST_F(IoSchedulerTest, DelCancelNonExistentFd) {
  IoScheduler scheduler(1, "test");
  // Random large FD
  EXPECT_EQ(scheduler.del_event(9999, Channel::kRead), 0);
  EXPECT_EQ(scheduler.cancel_event(9999, Channel::kRead), 0);
  EXPECT_EQ(scheduler.cancel_all(9999), 0);
}

TEST_F(IoSchedulerTest, TriggerEventNonExistent) {
  IoScheduler scheduler(1, "test");
  scheduler.trigger_event(9999, Channel::kRead);
}

TEST_F(IoSchedulerTest, ReleaseFdCleansStaleStateForFdReuse) {
  IoScheduler scheduler(1, "test");

  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  const int stale_fd = fds[0];
  const int peer_fd = fds[1];

  ASSERT_EQ(scheduler.add_event(stale_fd, Channel::kRead), 0);

  // 模拟内核层 fd 被关闭移除（epoll项自动消失），但用户态仍可能保留旧映射。
  ASSERT_EQ(::close(stale_fd), 0);

  // 显式释放调度器中的残留状态。
  EXPECT_EQ(scheduler.release_fd(stale_fd), 0);

  // 复用相同 fd 号。
  int dup_fd = ::dup(peer_fd);
  ASSERT_GE(dup_fd, 0);
  if (dup_fd != stale_fd) {
    ASSERT_EQ(::dup2(dup_fd, stale_fd), stale_fd);
    ASSERT_EQ(::close(dup_fd), 0);
  }

  // 复用后重新注册事件，不应因残留状态误走 MOD 失败。
  EXPECT_EQ(scheduler.add_event(stale_fd, Channel::kRead), 0);

  (void)scheduler.release_fd(stale_fd);
  ASSERT_EQ(::close(stale_fd), 0);
  ASSERT_EQ(::close(peer_fd), 0);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
