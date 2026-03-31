#include "znet/server.h"

#include <gtest/gtest.h>

namespace znet {
namespace {

class FakeServer final : public Server {
 public:
  bool start_result = true;
  int start_calls = 0;
  int stop_calls = 0;

 protected:
  bool do_start() override {
    ++start_calls;
    return start_result;
  }

  void do_stop() override { ++stop_calls; }
};

TEST(ServerUnitTest, StartStopTransitionsState) {
  FakeServer server;
  EXPECT_FALSE(server.is_running());

  EXPECT_TRUE(server.start());
  EXPECT_TRUE(server.is_running());
  EXPECT_EQ(server.start_calls, 1);

  server.stop();
  EXPECT_FALSE(server.is_running());
  EXPECT_EQ(server.stop_calls, 1);
}

TEST(ServerUnitTest, FailedStartRollsBackRunningState) {
  FakeServer server;
  server.start_result = false;

  EXPECT_FALSE(server.start());
  EXPECT_FALSE(server.is_running());
  EXPECT_EQ(server.start_calls, 1);

  server.stop();
  EXPECT_EQ(server.stop_calls, 0);
}

TEST(ServerUnitTest, RepeatedStartStopIsIdempotent) {
  FakeServer server;

  EXPECT_TRUE(server.start());
  EXPECT_TRUE(server.start());
  EXPECT_EQ(server.start_calls, 1);

  server.stop();
  server.stop();
  EXPECT_EQ(server.stop_calls, 1);
}

}  // namespace
}  // namespace znet
