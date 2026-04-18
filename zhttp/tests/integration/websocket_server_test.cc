#include "zhttp/http_server_builder.h"
#include "zhttp/websocket.h"
#include "zhttp/zhttp_logger.h"

#include <arpa/inet.h>
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
    const std::string response = recv_once_with_timeout(client_fd, 1000);
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
        .websocket("/ws", WebSocketCallbacks{},
                   WebSocketOptions{kDefaultWebSocketMaxMessageSize,
                                    {"superchat"}});

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
    const std::string response = recv_once_with_timeout(client_fd, 1000);
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

    ASSERT_TRUE(
        send_all(client_fd, build_masked_client_frame(WebSocketOpcode::kPing, "hb")));
    const std::string pong_response = recv_once_with_timeout(client_fd, 1000);
    ASSERT_GE(pong_response.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(pong_response[0]), 0x8A);
    EXPECT_EQ(static_cast<uint8_t>(pong_response[1]), 0x02);
    EXPECT_EQ(pong_response.substr(2, 2), "hb");

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));
    close_payload.append("bye");
    ASSERT_TRUE(send_all(
        client_fd, build_masked_client_frame(WebSocketOpcode::kClose, close_payload)));

    const std::string close_response = recv_once_with_timeout(client_fd, 1000);
    ASSERT_GE(close_response.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(close_response[0] & 0x0F), 0x08);

    ::close(client_fd);
}

} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
