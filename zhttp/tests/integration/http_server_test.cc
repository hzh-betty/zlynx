#include "http_server.h"
#include "http_server_builder.h"
#include "zhttp_logger.h"

#include <arpa/inet.h>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace zhttp;

namespace {

class ScopedServer {
public:
  explicit ScopedServer(std::shared_ptr<HttpServer> server)
      : server_(std::move(server)) {}

  ~ScopedServer() {
    if (server_) {
      server_->stop();
    }
  }

private:
  std::shared_ptr<HttpServer> server_;
};

uint16_t find_free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }

  const uint16_t port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

int connect_with_retry(uint16_t port, int retry_count, int retry_delay_ms) {
  for (int attempt = 0; attempt < retry_count; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
      return fd;
    }

    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
  }

  return -1;
}

bool send_all(int fd, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

std::string recv_until_close(int fd, int timeout_ms) {
  std::string response;
  char buffer[1024];

  while (true) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP;

    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0) {
      break;
    }

    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }

    response.append(buffer, static_cast<size_t>(n));
  }

  return response;
}

} // namespace

// 注意：集成测试需要网络环境，可能需要特殊配置
// 这里提供基本的路由和中间件测试

TEST(HttpServerIntegrationTest, RouteRegistration) {
  bool handler_called = false;
  Router router;

  router.get("/test",
             [&handler_called](const HttpRequest::ptr &, HttpResponse &resp) {
               handler_called = true;
               resp.status(HttpStatus::OK).text("OK");
             });

  // 模拟路由匹配
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/test");
  HttpResponse response;

  bool found = router.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_TRUE(handler_called);
  EXPECT_EQ(response.status_code(), HttpStatus::OK);
}

TEST(HttpServerIntegrationTest, MiddlewareIntegration) {
  // 创建一个简单的中间件
  class TestMiddleware : public Middleware {
  public:
    TestMiddleware(bool &before_called, bool &after_called)
        : before_called_(before_called), after_called_(after_called) {}

    bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
      before_called_ = true;
      resp.header("X-Test-Middleware", "before");
      return true;
    }

    void after(const HttpRequest::ptr &, HttpResponse &resp) override {
      after_called_ = true;
      resp.header("X-Test-After", "after");
    }

  private:
    bool &before_called_;
    bool &after_called_;
  };

  bool before_called = false;
  bool after_called = false;
  Router router;

  router.use(std::make_shared<TestMiddleware>(before_called, after_called));
  router.get("/middleware-test",
             [](const HttpRequest::ptr &, HttpResponse &resp) {
               resp.status(HttpStatus::OK).text("OK");
             });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/middleware-test");
  HttpResponse response;

  router.route(request, response);

  EXPECT_TRUE(before_called);
  EXPECT_TRUE(after_called);
  EXPECT_EQ(response.headers().at("X-Test-Middleware"), "before");
  EXPECT_EQ(response.headers().at("X-Test-After"), "after");
}

TEST(HttpServerIntegrationTest, NotFoundRoute) {
  Router router;
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/nonexistent");
  HttpResponse response;

  bool found = router.route(request, response);

  EXPECT_FALSE(found);
  EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST(HttpServerIntegrationTest, KeepsParserStateAcrossSplitPackets) {
  const uint16_t port = find_free_port();
  ASSERT_NE(port, 0);

  HttpServerBuilder builder;
  builder.listen("127.0.0.1", port)
      .threads(1)
      .log_level("error")
      .get("/split", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("split-ok");
      });

  auto server = builder.build();
  ASSERT_TRUE(server);
  ScopedServer guard(server);
  ASSERT_TRUE(server->start());

  const int client_fd = connect_with_retry(port, 20, 25);
  ASSERT_GE(client_fd, 0);

  const std::string first_chunk =
      "GET /split HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close";
  const std::string second_chunk = "\r\n\r\n";

  ASSERT_TRUE(send_all(client_fd, first_chunk));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_TRUE(send_all(client_fd, second_chunk));

  const std::string response = recv_until_close(client_fd, 1000);
  ::close(client_fd);

  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos)
      << response;
  EXPECT_NE(response.find("split-ok"), std::string::npos) << response;
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
