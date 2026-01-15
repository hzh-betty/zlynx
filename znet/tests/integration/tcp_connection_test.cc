/**
 * @file tcp_connection_test.cc
 * @brief TcpConnection 集成测试
 */

#include "address.h"
#include "io/io_scheduler.h"
#include "socket.h"
#include "tcp_connection.h"
#include "znet_logger.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace znet;
using namespace zcoroutine;

class TcpConnectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    scheduler_ = std::make_shared<IoScheduler>(1, "TcpConnTest", false);
    scheduler_->start();
  }

  void TearDown() override {
    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }
  }

  IoScheduler::ptr scheduler_;
};

// 测试 TcpConnection 创建
TEST_F(TcpConnectionTest, CreateConnection) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  EXPECT_EQ(conn->name(), "test-conn");
  EXPECT_EQ(conn->local_address(), local_addr);
  EXPECT_EQ(conn->peer_address(), peer_addr);
  EXPECT_EQ(conn->state(), TcpConnection::State::Connecting);
}

// 测试连接建立回调
TEST_F(TcpConnectionTest, ConnectionEstablished) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  auto sock = std::make_shared<Socket>(sv[0]);
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  std::atomic<bool> callback_called{false};
  conn->set_connection_callback(
      [&callback_called](const TcpConnectionPtr &) { callback_called = true; });

  conn->connect_established();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(conn->state(), TcpConnection::State::Connected);

  // 清理另一端的socket
  ::close(sv[1]);
}

// 测试 Buffer 读写
TEST_F(TcpConnectionTest, BufferReadWrite) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  // 测试输入缓冲区
  Buffer *input = conn->input_buffer();
  ASSERT_NE(input, nullptr);
  input->append("Hello", 5);
  EXPECT_EQ(input->readable_bytes(), 5u);

  // 测试输出缓冲区
  Buffer *output = conn->output_buffer();
  ASSERT_NE(output, nullptr);
  output->append("World", 5);
  EXPECT_EQ(output->readable_bytes(), 5u);
}

// 测试设置回调函数
TEST_F(TcpConnectionTest, SetCallbacks) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  std::atomic<int> callback_count{0};

  conn->set_connection_callback(
      [&callback_count](const TcpConnectionPtr &) { callback_count++; });

  conn->set_message_callback([&callback_count](const TcpConnectionPtr &,
                                               Buffer *) { callback_count++; });

  conn->set_write_complete_callback(
      [&callback_count](const TcpConnectionPtr &) { callback_count++; });

  conn->set_close_callback(
      [&callback_count](const TcpConnectionPtr &) { callback_count++; });

  // 回调设置不应崩溃
  EXPECT_EQ(callback_count, 0);
}

// 测试 TCP_NODELAY 设置
TEST_F(TcpConnectionTest, SetTcpNoDelay) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  // 不应该崩溃
  conn->set_tcp_no_delay(true);
  conn->set_tcp_no_delay(false);
}

// 测试 KeepAlive 设置
TEST_F(TcpConnectionTest, SetKeepAlive) {
  auto local_addr = std::make_shared<IPv4Address>("127.0.0.1", 9000);
  auto peer_addr = std::make_shared<IPv4Address>("127.0.0.1", 9001);

  auto sock = Socket::create_tcp();
  ASSERT_NE(sock, nullptr);

  auto conn = std::make_shared<TcpConnection>("test-conn", sock, local_addr,
                                              peer_addr, scheduler_.get());

  // 不应该崩溃
  conn->set_keep_alive(true);
  conn->set_keep_alive(false);
}

int main(int argc, char **argv) {
  znet::init_logger(zlog::LogLevel::value::DEBUG);
  
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
