/**
 * @file socket_test.cc
 * @brief Socket 单元测试
 */

#include "address.h"
#include "socket.h"
#include "znet_logger.h"
#include <gtest/gtest.h>

using namespace znet;

class SocketTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// 测试创建 TCP Socket
TEST_F(SocketTest, CreateTCPSocket) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);
  EXPECT_TRUE(sock->is_valid());
  EXPECT_EQ(sock->family(), AF_INET);
  EXPECT_EQ(sock->type(), SOCK_STREAM);
}

// 测试创建 UDP Socket
TEST_F(SocketTest, CreateUDPSocket) {
  auto sock = Socket::create_udp();
  ASSERT_NE(sock, nullptr);
  EXPECT_TRUE(sock->is_valid());
  EXPECT_EQ(sock->family(), AF_INET);
  EXPECT_EQ(sock->type(), SOCK_DGRAM);
}

// 测试 TCP IPv6 Socket
TEST_F(SocketTest, CreateTCPIPv6Socket) {
  auto sock = Socket::create_tcp_v6();
  ASSERT_NE(sock, nullptr);
  EXPECT_TRUE(sock->is_valid());
  EXPECT_EQ(sock->family(), AF_INET6);
}

// 测试 Socket 选项设置
TEST_F(SocketTest, SetSocketOption) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  // 设置 SO_REUSEADDR
  EXPECT_TRUE(sock->set_reuse_addr(true));
}

// 测试 TCP_NODELAY
TEST_F(SocketTest, SetTcpNoDelay) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  EXPECT_TRUE(sock->set_tcp_nodelay(true));
}

// 测试 SO_KEEPALIVE
TEST_F(SocketTest, SetKeepAlive) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  EXPECT_TRUE(sock->set_keep_alive(true));
}

// 测试绑定
TEST_F(SocketTest, Bind) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto addr =
      std::make_shared<IPv4Address>("127.0.0.1", 0); // 端口0表示让系统分配
  ASSERT_NE(addr, nullptr);

  EXPECT_TRUE(sock->bind(addr));

  // 获取实际绑定的地址
  auto bound_addr = sock->get_local_address();
  ASSERT_NE(bound_addr, nullptr);
}

// 测试监听
TEST_F(SocketTest, Listen) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  ASSERT_NE(addr, nullptr);

  EXPECT_TRUE(sock->bind(addr));
  EXPECT_TRUE(sock->listen(128));
}

// 测试获取本地地址（未绑定）
TEST_F(SocketTest, GetLocalAddressUnbound) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  // 未绑定的socket获取地址
  auto addr = sock->get_local_address();
  // 可能返回nullptr或有效地址，取决于实现
}

// 测试关闭写端
TEST_F(SocketTest, ShutdownWrite) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  // 只测试调用不会崩溃
  sock->shutdown_write();
}

// 测试 set_non_blocking
TEST_F(SocketTest, SetNonBlocking) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  EXPECT_TRUE(sock->set_non_blocking(true));
  EXPECT_TRUE(sock->set_non_blocking(false));
}

// 测试 set_reuse_port
TEST_F(SocketTest, SetReusePort) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  EXPECT_TRUE(sock->set_reuse_port(true));
}

// 测试关闭
TEST_F(SocketTest, Close) {
  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);
  EXPECT_TRUE(sock->is_valid());

  sock->close();
  EXPECT_FALSE(sock->is_valid());
}

int main(int argc, char **argv) {
  znet::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
