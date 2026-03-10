#include "http_server_builder.h"
#include "http_server.h"
#include "zhttp_logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

int find_free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }

  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

int connect_to_server(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  timeval tv {};
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

bool peer_closed_or_reset(int fd) {
  char buffer[256];
  errno = 0;
  const ssize_t bytes = ::recv(fd, buffer, sizeof(buffer), 0);
  if (bytes == 0) {
    return true;
  }
  if (bytes < 0 &&
      (errno == ECONNRESET || errno == EPIPE || errno == ETIMEDOUT)) {
    return true;
  }
  return false;
}

class ScopedHttpServer {
public:
  ScopedHttpServer(uint64_t read_timeout_ms, uint64_t write_timeout_ms,
                   uint64_t keepalive_timeout_ms) {
    port_ = find_free_port();
    EXPECT_GT(port_, 0);

    zhttp::HttpServerBuilder builder;
    builder.listen("127.0.0.1", static_cast<uint16_t>(port_))
        .threads(1)
        .read_timeout(read_timeout_ms)
        .write_timeout(write_timeout_ms)
        .keepalive_timeout(keepalive_timeout_ms)
        .get("/", [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp) {
          resp.status(zhttp::HttpStatus::OK).text("OK");
        });

    server_ = builder.build();
    if (!server_->start()) {
      throw std::runtime_error("failed to start test server");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }

  ~ScopedHttpServer() {
    if (server_) {
      server_->stop();
    }
  }

  int port() const { return port_; }

private:
  int port_ = -1;
  std::shared_ptr<zhttp::HttpServer> server_;
};

} // namespace

TEST(HttpServerTimeoutIntegrationTest, ClosesIdleConnectionOnReadTimeout) {
  ScopedHttpServer server(100, 0, 0);

  const int client_fd = connect_to_server(server.port());
  ASSERT_GE(client_fd, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_TRUE(peer_closed_or_reset(client_fd));

  ::close(client_fd);
}

TEST(HttpServerTimeoutIntegrationTest, ClosesKeepAliveConnectionWhenIdle) {
  ScopedHttpServer server(1000, 0, 120);

  const int client_fd = connect_to_server(server.port());
  ASSERT_GE(client_fd, 0);

  const std::string request =
      "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_GT(::send(client_fd, request.data(), request.size(), 0), 0);

  char response[1024];
  ASSERT_GT(::recv(client_fd, response, sizeof(response), 0), 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_TRUE(peer_closed_or_reset(client_fd));

  ::close(client_fd);
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}