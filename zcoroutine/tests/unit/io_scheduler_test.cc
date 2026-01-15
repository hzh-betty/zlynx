#include "io/io_scheduler.h"
#include "util/zcoroutine_logger.h"
#include <gtest/gtest.h>

using namespace zcoroutine;

class IoSchedulerTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(IoSchedulerTest, AddInvalidFd) {
  IoScheduler scheduler(1, "test");

  // FD -1 is invalid
  EXPECT_EQ(scheduler.add_event(-1, FdContext::kRead), -1);
}

TEST_F(IoSchedulerTest, DelCancelNonExistentFd) {
  IoScheduler scheduler(1, "test");
  // Random large FD
  EXPECT_EQ(scheduler.del_event(9999, FdContext::kRead), 0);
  EXPECT_EQ(scheduler.cancel_event(9999, FdContext::kRead), 0);
  EXPECT_EQ(scheduler.cancel_all(9999), 0);
}

TEST_F(IoSchedulerTest, TriggerEventNonExistent) {
  IoScheduler scheduler(1, "test");
  scheduler.trigger_event(9999, FdContext::kRead);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
