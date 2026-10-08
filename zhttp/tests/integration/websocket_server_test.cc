#include "../test_support.h"
#include "zhttp/http_server_builder.h"
#include "zhttp/websocket/websocket_handler.h"
#include "zhttp/zhttp_logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace zhttp {
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

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
            0) {
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

std::string recv_once_with_timeout(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP;

    if (::poll(&pfd, 1, timeout_ms) <= 0) {
        return "";
    }

    char buffer[2048];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        return "";
    }
    return std::string(buffer, static_cast<size_t>(n));
}

std::string recv_until_closed_with_timeout(int fd, int timeout_ms) {
    // 错误响应由头部与正文分批写出，TCP 不保证一次 recv 收齐。
    // 两个握手拒绝场景都要求关闭连接，累计读取到 EOF，并共用总时限。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::string response;
    while (true) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            break;
        const auto wait_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                .count() + 1;
        pollfd ready{fd, POLLIN | POLLHUP, 0};
        const int result = ::poll(&ready, 1, static_cast<int>(wait_ms));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            break;
        char buffer[2048];
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            EXPECT_EQ(count, 0) << "读取错误响应时连接异常";
            return response;
        }
        response.append(buffer, static_cast<size_t>(count));
    }
    ADD_FAILURE() << "错误响应未在总时限内关闭连接";
    return response;
}

std::string build_masked_client_frame(WebSocketOpcode opcode,
                                      const std::string &payload,
                                      bool fin = true) {
    std::string frame;
    const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                               static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(first));

    const uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    frame.append(reinterpret_cast<const char *>(mask_key), sizeof(mask_key));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));
    }

    return frame;
}

} // namespace

TEST(WebSocketServerIntegrationTest, UpgradesAndEchoesTextFrame) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .websocket("/ws",
                   WebSocketCallbacks{
                       {},
                       [](const WebSocketConnection::ptr &conn,
                          std::string &&message, WebSocketMessageType type) {
                           if (type == WebSocketMessageType::kText) {
                               conn->send_text(message);
                           }
                       },
                       {},
                       {}},
                   WebSocketOptions{kDefaultWebSocketMaxMessageSize,
                                    {"superchat", "chat"}});

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string handshake_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT_TRUE(send_all(client_fd, handshake_request));
    const std::string handshake_response =
        recv_once_with_timeout(client_fd, 1000);
    ASSERT_NE(handshake_response.find("101 Switching Protocols"),
              std::string::npos)
        << handshake_response;
    ASSERT_NE(handshake_response.find("Sec-WebSocket-Protocol: superchat"),
              std::string::npos)
        << handshake_response;

    ASSERT_TRUE(send_all(
        client_fd, build_masked_client_frame(WebSocketOpcode::kText, "hello")));

    const std::string ws_response = recv_once_with_timeout(client_fd, 1000);
    ASSERT_GE(ws_response.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ws_response[0]), 0x81);
    EXPECT_EQ(static_cast<uint8_t>(ws_response[1]), 0x05);
    EXPECT_EQ(ws_response.substr(2), "hello");

    ::close(client_fd);
}

TEST(WebSocketServerIntegrationTest, RejectsUnsupportedWebSocketVersion) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .websocket("/ws", WebSocketCallbacks{}, WebSocketOptions{});

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string handshake_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 12\r\n"
        "\r\n";

    ASSERT_TRUE(send_all(client_fd, handshake_request));
    const std::string response = recv_until_closed_with_timeout(client_fd, 1000);
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
    EXPECT_NE(response.find("Sec-WebSocket-Version: 13"), std::string::npos)
        << response;

    ::close(client_fd);
}

TEST(WebSocketServerIntegrationTest, RejectsIncompatibleSubprotocolRequest) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .websocket(
            "/ws", WebSocketCallbacks{},
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {"superchat"}});

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string handshake_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT_TRUE(send_all(client_fd, handshake_request));
    const std::string response = recv_until_closed_with_timeout(client_fd, 1000);
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
    EXPECT_NE(response.find("No compatible Sec-WebSocket-Protocol"),
              std::string::npos)
        << response;

    ::close(client_fd);
}

TEST(WebSocketServerIntegrationTest, RespondsWithPongAndCloseFrame) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .websocket("/ws", WebSocketCallbacks{}, WebSocketOptions{});

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string handshake_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT_TRUE(send_all(client_fd, handshake_request));
    const std::string handshake_response =
        recv_once_with_timeout(client_fd, 1000);
    ASSERT_NE(handshake_response.find("101 Switching Protocols"),
              std::string::npos)
        << handshake_response;

    ASSERT_TRUE(send_all(
        client_fd, build_masked_client_frame(WebSocketOpcode::kPing, "hb")));
    const std::string pong_response = recv_once_with_timeout(client_fd, 1000);
    ASSERT_GE(pong_response.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(pong_response[0]), 0x8A);
    EXPECT_EQ(static_cast<uint8_t>(pong_response[1]), 0x02);
    EXPECT_EQ(pong_response.substr(2, 2), "hb");

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));
    close_payload.append("bye");
    ASSERT_TRUE(
        send_all(client_fd, build_masked_client_frame(WebSocketOpcode::kClose,
                                                      close_payload)));

    const std::string close_response = recv_once_with_timeout(client_fd, 1000);
    ASSERT_GE(close_response.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(close_response[0] & 0x0F), 0x08);

    ::close(client_fd);
}

TEST(WebSocketServerIntegrationTest,
     SynchronousStreamUpgradesBufferedRequestAndPreservesMiddleware) {
    class Decorate : public Middleware {
      public:
        void after(HttpContext &context) override {
            context.response()
                .header("X-Middleware", "seen")
                .set_cookie("session", "value");
        }
    };
    const auto port = find_free_port();
    ASSERT_NE(port, 0);
    std::atomic<int> completed{0}, opened{0}, stream_completed{0};
    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .use(std::make_shared<Decorate>())
        .get(
            "/stream",
            [&](HttpContext &context) {
                context.on_complete([&](CompletionResult result) {
                    EXPECT_EQ(result, CompletionResult::Completed);
                    ++stream_completed;
                });
                context.response().stream([](char *, size_t) { return 0U; });
            })
        .get("/ws", [&](HttpContext &context) {
            context.on_complete([&](CompletionResult result) {
                EXPECT_EQ(result, CompletionResult::Upgraded);
                ++completed;
            });
            WebSocketCallbacks callbacks;
            callbacks.on_open =
                [&](const WebSocketConnection::ptr &,
                    const std::shared_ptr<const HttpRequest> &) {
                    EXPECT_EQ(completed.load(), 1);
                    EXPECT_EQ(stream_completed.load(), 1);
                    ++opened;
                };
            callbacks.on_message =
                [](const WebSocketConnection::ptr &connection,
                   std::string &&message, WebSocketMessageType) {
                    EXPECT_EQ(message, "hello");
                    EXPECT_TRUE(connection->send_text(message));
                };
            context.upgrade_to_websocket(std::move(callbacks));
        });
    auto server = builder.build();
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    int fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(fd, 0);
    std::string input =
        "GET /stream HTTP/1.1\r\nHost: test\r\n\r\nGET /ws HTTP/1.1\r\nHost: "
        "test\r\nConnection: Upgrade\r\nUpgrade: "
        "websocket\r\nSec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    input += build_masked_client_frame(WebSocketOpcode::kText, "hel", false) +
             build_masked_client_frame(WebSocketOpcode::kPing, "p") +
             build_masked_client_frame(WebSocketOpcode::kContinuation, "lo");
    ASSERT_TRUE(send_all(fd, input));
    std::string output;
    while (output.find(std::string("\x81\x05hello", 7)) == std::string::npos) {
        auto bytes = recv_once_with_timeout(fd, 1000);
        ASSERT_FALSE(bytes.empty()) << output;
        output += bytes;
    }
    const auto upgrade_start =
        output.find("0\r\n\r\nHTTP/1.1 101 Switching Protocols");
    ASSERT_NE(upgrade_start, std::string::npos);
    const auto handshake = output.substr(upgrade_start + 5);
    const auto headers = handshake.find("\r\n\r\n");
    ASSERT_NE(headers, std::string::npos);
    EXPECT_NE(handshake.find("X-Middleware: seen\r\n"), std::string::npos);
    EXPECT_NE(handshake.find("Set-Cookie: session=value;"), std::string::npos);
    EXPECT_EQ(handshake.substr(headers + 4),
              std::string("\x8a\x01p\x81\x05hello", 10));
    EXPECT_EQ(completed.load(), 1);
    EXPECT_EQ(opened.load(), 1);
    ::close(fd);
}
TEST(WebSocketServerIntegrationTest, LocalCloseWaitsForPeerWithBoundedTimeout) {
    const auto port = find_free_port();
    ASSERT_NE(port, 0);
    std::atomic<int> closed{0};
    WebSocketCallbacks callbacks;
    callbacks.on_open = [](const WebSocketConnection::ptr &connection,
                           const std::shared_ptr<const HttpRequest> &) {
        EXPECT_TRUE(
            connection->close(WebSocketCloseCode::kNormalClosure, "bye"));
        EXPECT_EQ(connection->state(), WebSocketConnection::State::Closing);
        EXPECT_FALSE(connection->send_text("late"));
    };
    callbacks.on_close = [&](const WebSocketConnection::ptr &connection,
                             uint16_t, const std::string &) {
        EXPECT_EQ(connection->state(), WebSocketConnection::State::Closed);
        ++closed;
        throw std::runtime_error("cleanup");
    };
    WebSocketOptions options;
    options.close_timeout_ms = 40;
    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .websocket("/ws", std::move(callbacks), options);
    auto server = builder.build();
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    int fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_all(
        fd, "GET /ws HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: "
            "websocket\r\nSec-WebSocket-Key: "
            "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    std::string output;
    const auto begin = std::chrono::steady_clock::now();
    while (true) {
        pollfd ready{fd, POLLIN | POLLHUP, 0};
        ASSERT_GT(::poll(&ready, 1, 1000), 0);
        char buffer[2048];
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        ASSERT_GE(count, 0);
        if (!count)
            break;
        output.append(buffer, count);
    }
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - begin)
                  .count(),
              1000);
    EXPECT_NE(output.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(output.find(std::string("\x88\x05\x03\xe8"
                                      "bye",
                                      7)),
              std::string::npos);
    server->stop();
    EXPECT_EQ(closed.load(), 1);
    ::close(fd);
}

} // namespace zhttp

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
