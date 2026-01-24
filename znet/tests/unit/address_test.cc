/**
 * @file address_test.cc
 * @brief Address 单元测试
 */

#include "address.h"
#include "znet_logger.h"
#include <arpa/inet.h>
#include <gtest/gtest.h>

using namespace znet;

class AddressTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// 测试 IPv4Address 创建
TEST_F(AddressTest, IPv4AddressCreate) {
  auto addr = std::make_shared<IPv4Address>("127.0.0.1", 8080);
  ASSERT_NE(addr, nullptr);
  EXPECT_EQ(addr->family(), AF_INET);
  EXPECT_EQ(addr->to_string(), "127.0.0.1:8080");
}

// 测试 IPv4Address 端口
TEST_F(AddressTest, IPv4AddressPort) {
  auto addr = std::make_shared<IPv4Address>("192.168.1.1", 9000);
  ASSERT_NE(addr, nullptr);
  EXPECT_EQ(addr->port(), 9000u);

  addr->set_port(9001);
  EXPECT_EQ(addr->port(), 9001u);
}

// 测试 IPv6Address 创建
TEST_F(AddressTest, IPv6AddressCreate) {
  auto addr = std::make_shared<IPv6Address>("::1", 8080);
  ASSERT_NE(addr, nullptr);
  EXPECT_EQ(addr->family(), AF_INET6);
}

// 测试 IPv6Address 端口
TEST_F(AddressTest, IPv6AddressPort) {
  auto addr = std::make_shared<IPv6Address>("fe80::1", 9000);
  ASSERT_NE(addr, nullptr);
  EXPECT_EQ(addr->port(), 9000u);

  addr->set_port(9001);
  EXPECT_EQ(addr->port(), 9001u);
}

// 测试 Address::lookup
TEST_F(AddressTest, LookupAddress) {
  auto addrs = Address::lookup("localhost", 0, AF_INET);
  // localhost 可能解析成功或失败，取决于系统配置
  // 我们只测试调用不会崩溃
}

// 测试 0.0.0.0 (INADDR_ANY)
TEST_F(AddressTest, AnyAddress) {
  auto addr = std::make_shared<IPv4Address>("0.0.0.0", 8080);
  ASSERT_NE(addr, nullptr);
  EXPECT_EQ(addr->to_string(), "0.0.0.0:8080");
}

// 测试默认构造
TEST_F(AddressTest, DefaultConstruction) {
  IPv4Address addr;
  EXPECT_EQ(addr.family(), AF_INET);
  EXPECT_EQ(addr.port(), 0u);
}

// 测试地址比较
TEST_F(AddressTest, AddressComparison) {
  auto addr1 = std::make_shared<IPv4Address>("127.0.0.1", 8080);
  auto addr2 = std::make_shared<IPv4Address>("127.0.0.1", 8080);
  auto addr3 = std::make_shared<IPv4Address>("127.0.0.1", 8081);

  ASSERT_NE(addr1, nullptr);
  ASSERT_NE(addr2, nullptr);
  ASSERT_NE(addr3, nullptr);

  EXPECT_EQ(addr1->to_string(), addr2->to_string());
  EXPECT_NE(addr1->to_string(), addr3->to_string());
}

// 测试 sockaddr 长度
TEST_F(AddressTest, SockaddrLen) {
  IPv4Address addr4;
  EXPECT_EQ(addr4.sockaddr_len(), sizeof(sockaddr_in));

  IPv6Address addr6;
  EXPECT_EQ(addr6.sockaddr_len(), sizeof(sockaddr_in6));
}

// 测试 sockaddr 指针
TEST_F(AddressTest, SockaddrPtr) {
  IPv4Address addr("192.168.1.1", 8080);
  const sockaddr *ptr = addr.sockaddr_ptr();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(ptr->sa_family, AF_INET);
}

int main(int argc, char **argv) {

  znet::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
