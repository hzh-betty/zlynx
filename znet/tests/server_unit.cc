#include "znet/address.h"
#include "znet/tcp_server.h"

#include <gtest/gtest.h>

#include "zcoroutine/sched.h"

namespace znet {
namespace {

class TcpServerLifecycleUnitTest : public ::testing::Test {
 public:
  void TearDown() override { zcoroutine::shutdown(); }
};

TEST_F(TcpServerLifecycleUnitTest, StartStopTransitionsState) {
  zcoroutine::init(2);
  auto server = std::make_shared<TcpServer>(
      std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
  ASSERT_NE(server, nullptr);

  EXPECT_FALSE(server->is_running());
  EXPECT_TRUE(server->start());
  EXPECT_TRUE(server->is_running());

  server->stop();
  EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, FailedStartRollsBackRunningState) {
  auto server = std::make_shared<TcpServer>(Address::ptr{}, 16);
  ASSERT_NE(server, nullptr);

  EXPECT_FALSE(server->start());
  EXPECT_FALSE(server->is_running());

  server->stop();
  EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, RepeatedStartStopIsIdempotent) {
  zcoroutine::init(1);
  auto server = std::make_shared<TcpServer>(
      std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
  ASSERT_NE(server, nullptr);

  EXPECT_TRUE(server->start());
  EXPECT_TRUE(server->start());
  EXPECT_TRUE(server->is_running());

  server->stop();
  server->stop();
  EXPECT_FALSE(server->is_running());
}

}  // namespace
}  // namespace znet
