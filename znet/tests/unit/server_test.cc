#include "znet/address.h"
#include "znet/tcp_server.h"

#include <gtest/gtest.h>

#include "zco/sched.h"

namespace znet {
namespace {

class TcpServerLifecycleUnitTest : public ::testing::Test {
  public:
    void TearDown() override { zco::shutdown(); }
};

TEST_F(TcpServerLifecycleUnitTest, StartStopTransitionsState) {
    zco::init(2);
    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);
    server->set_thread_count(1);

    EXPECT_FALSE(server->is_running());
    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->is_running());

    server->stop();
    EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, FailedStartRollsBackRunningState) {
    auto server = std::make_shared<TcpServer>(Address::ptr{}, 16);
    ASSERT_NE(server, nullptr);
    server->set_thread_count(1);

    EXPECT_FALSE(server->start());
    EXPECT_FALSE(server->is_running());

    server->stop();
    EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, RepeatedStartStopIsIdempotent) {
    zco::init(1);
    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);
    server->set_thread_count(1);

    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->is_running());

    server->stop();
    server->stop();
    EXPECT_FALSE(server->is_running());
}

} // namespace
} // namespace znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
