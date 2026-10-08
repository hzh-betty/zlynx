/**
 * @file server_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include "znet/address.h"
#include "znet/tcp_server.h"

#include <gtest/gtest.h>

#include "znet/znet_logger.h"

#include "zco/coroutine.h"

namespace znet {
namespace {

class TcpServerLifecycleUnitTest : public ::testing::Test {
  public:
    void TearDown() override {}
};

TEST_F(TcpServerLifecycleUnitTest, StartStopTransitionsState) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    auto server = std::make_shared<TcpServer>(
        runtime, std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    EXPECT_FALSE(server->is_running());
    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->is_running());

    server->stop();
    EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, FailedStartRollsBackRunningState) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    auto server = std::make_shared<TcpServer>(runtime, Address::ptr{}, 16);
    ASSERT_NE(server, nullptr);

    EXPECT_FALSE(server->start());
    EXPECT_FALSE(server->is_running());

    server->stop();
    EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, RepeatedStartStopIsIdempotent) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto server = std::make_shared<TcpServer>(
        runtime, std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->start());
    EXPECT_TRUE(server->is_running());

    server->stop();
    server->stop();
    EXPECT_FALSE(server->is_running());
}

TEST_F(TcpServerLifecycleUnitTest, StoppingOneServerDoesNotStopSharedRuntime) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    auto first = std::make_shared<TcpServer>(
        runtime, std::make_shared<IPv4Address>("127.0.0.1", 0));
    auto second = std::make_shared<TcpServer>(
        runtime, std::make_shared<IPv4Address>("127.0.0.1", 0));
    ASSERT_TRUE(first->start());
    ASSERT_TRUE(second->start());
    first->stop();
    EXPECT_TRUE(second->is_running());
    EXPECT_TRUE(runtime.executor(0).valid());
    auto task = runtime.spawn([] {});
    ASSERT_TRUE(task);
    EXPECT_TRUE(task.value().join());
    second->stop();
}

} // namespace
} // namespace znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    znet::init_logger();
    return RUN_ALL_TESTS();
}
