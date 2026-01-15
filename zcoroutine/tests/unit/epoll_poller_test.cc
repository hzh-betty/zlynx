#include "io/epoll_poller.h"
#include "util/zcoroutine_logger.h"
#include <gtest/gtest.h>
#include <sys/epoll.h>

using namespace zcoroutine;

class EpollPollerTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(EpollPollerTest, AddInvalidFd) {
  EpollPoller poller(10);
  // -1 is invalid
  EXPECT_EQ(poller.add_event(-1, EPOLLIN, nullptr), -1);
}

TEST_F(EpollPollerTest, ModInvalidFd) {
  EpollPoller poller(10);
  EXPECT_EQ(poller.mod_event(-1, EPOLLIN, nullptr), -1);
}

TEST_F(EpollPollerTest, DelInvalidFd) {
  EpollPoller poller(10);
  EXPECT_EQ(poller.del_event(-1), 0);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
