#include "http_server.h"

#include "zhttp_logger.h"

#include "znet/socket.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

class HttpServerContextTestDouble : public HttpServer {
 public:
  explicit HttpServerContextTestDouble(znet::Address::ptr listen_address)
      : HttpServer(std::move(listen_address)) {}

  using HttpServer::on_close;
  using HttpServer::on_connection;
};

TEST(HttpServerContextTest, ParserContextLifecycleFollowsConnection) {
  auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
  HttpServerContextTestDouble server(listen_address);

  auto socket = znet::Socket::create_tcp();
  ASSERT_TRUE(socket != nullptr);

  auto conn = std::make_shared<znet::TcpConnection>(socket);
  ASSERT_TRUE(conn != nullptr);
  EXPECT_EQ(conn->context(), nullptr);

  server.on_connection(conn);
  EXPECT_NE(conn->context(), nullptr);

  server.on_close(conn);
  EXPECT_EQ(conn->context(), nullptr);
}

TEST(HttpServerContextTest, HttpServerRejectsInvalidTlsCertificate) {
  auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
  HttpServerContextTestDouble server(listen_address);

  EXPECT_FALSE(server.set_ssl_certificate("/tmp/not-exist-cert.pem",
                                          "/tmp/not-exist-key.pem"));
}

}  // namespace
}  // namespace zhttp

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
