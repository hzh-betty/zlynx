#include "../support/network_fixture.h"
#include "../support/server_fixture.h"
#include "zhttp/websocket/websocket_handler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>

namespace zhttp {
namespace {

using namespace zhttp::test;

std::string build_masked_client_frame(WebSocketOpcode opcode,
                                      const std::string &payload,
                                      bool fin = true) {
    std::string frame;
    const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                               static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(first));

    const uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        const size_t length_bytes = payload.size() <= 0xffff ? 2 : 8;
        frame.push_back(static_cast<char>(length_bytes == 2 ? 0xfe : 0xff));
        for (size_t i = length_bytes; i > 0; --i)
            frame.push_back(static_cast<char>(
                static_cast<uint64_t>(payload.size()) >> ((i - 1) * 8)));
    }
    frame.append(reinterpret_cast<const char *>(mask_key), sizeof(mask_key));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));
    }

    return frame;
}

} // namespace

TEST(WebSocketServerIntegrationTest, UpgradesAndEchoesTextFrame) {

    auto server = make_server();
    register_websocket(
        *server, "/ws",
        WebSocketCallbacks{{},
                           [](const WebSocketConnection::ptr &conn,
                              std::string &&message,
                              WebSocketMessageType type) {
                               if (type == WebSocketMessageType::kText) {
                                   conn->send_text(message);
                               }
                           },
                           {},
                           {}},
        WebSocketOptions{kDefaultWebSocketMaxMessageSize,
                         {"superchat", "chat"}});

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();
    SocketReader reader(client_fd, 1);

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
        reader.until("\r\n\r\n", zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    ASSERT_NE(handshake_response.find("101 Switching Protocols"),
              std::string::npos)
        << handshake_response;
    ASSERT_NE(handshake_response.find("Sec-WebSocket-Protocol: superchat"),
              std::string::npos)
        << handshake_response;

    ASSERT_TRUE(send_all(
        client_fd, build_masked_client_frame(WebSocketOpcode::kText, "hello")));

    const std::string ws_response =
        recv_websocket_frame(reader,
                             zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    ASSERT_GE(ws_response.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(ws_response[0]), 0x81);
    EXPECT_EQ(static_cast<uint8_t>(ws_response[1]), 0x05);
    EXPECT_EQ(ws_response.substr(2), "hello");

    for (const size_t size : {size_t{126}, size_t{65536}}) {
        SCOPED_TRACE(size);
        const std::string payload(size, 'x');
        ASSERT_TRUE(send_all(client_fd, build_masked_client_frame(
                                            WebSocketOpcode::kText, payload)));
        const auto echoed =
            recv_websocket_frame(reader,
                                 zco::Deadline::after(std::chrono::seconds(3)))
                .value();
        const std::string header =
            size == 126
                ? std::string("\x81\x7e\x00\x7e", 4)
                : std::string("\x81\x7f\x00\x00\x00\x00\x00\x01\x00\x00", 10);
        EXPECT_EQ(echoed, header + payload);
    }
}

TEST(WebSocketServerIntegrationTest, RejectsUnsupportedWebSocketVersion) {

    auto server = make_server();
    register_websocket(*server, "/ws", WebSocketCallbacks{},
                       WebSocketOptions{});

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();
    SocketReader reader(client_fd, 1);

    const std::string handshake_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 12\r\n"
        "\r\n";

    ASSERT_TRUE(send_all(client_fd, handshake_request));
    const std::string response =
        reader.until_closed(zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
    EXPECT_NE(response.find("Sec-WebSocket-Version: 13"), std::string::npos)
        << response;
}

TEST(WebSocketServerIntegrationTest, RejectsIncompatibleSubprotocolRequest) {

    auto server = make_server();
    register_websocket(
        *server, "/ws", WebSocketCallbacks{},
        WebSocketOptions{kDefaultWebSocketMaxMessageSize, {"superchat"}});

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();
    SocketReader reader(client_fd, 1);

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
    const std::string response =
        reader.until_closed(zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
    EXPECT_NE(response.find("No compatible Sec-WebSocket-Protocol"),
              std::string::npos)
        << response;
}

TEST(WebSocketServerIntegrationTest, RespondsWithPongAndCloseFrame) {

    auto server = make_server();
    register_websocket(*server, "/ws", WebSocketCallbacks{},
                       WebSocketOptions{});

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();
    SocketReader reader(client_fd, 1);

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
        reader.until("\r\n\r\n", zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    ASSERT_NE(handshake_response.find("101 Switching Protocols"),
              std::string::npos)
        << handshake_response;

    ASSERT_TRUE(send_all(
        client_fd, build_masked_client_frame(WebSocketOpcode::kPing, "hb")));
    const std::string pong_response =
        recv_websocket_frame(reader,
                             zco::Deadline::after(std::chrono::seconds(1)))
            .value();
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

    const std::string close_response =
        recv_websocket_frame(reader,
                             zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    EXPECT_EQ(close_response, std::string("\x88\x05", 2) + close_payload);
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

    std::atomic<int> completed{0}, opened{0}, stream_completed{0};
    auto server = make_server();
    server->use(std::make_shared<Decorate>());
    server->router().get("/stream", [&](HttpContext &context) {
        context.on_complete([&](CompletionResult result) {
            EXPECT_EQ(result, CompletionResult::Completed);
            ++stream_completed;
        });
        context.response().stream([](char *, size_t) { return 0U; });
    });
    server->router().get(
        "/ws", [&](HttpContext &context) {
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
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    SocketReader reader(fd, 1);
    std::string input =
        "GET /stream HTTP/1.1\r\nHost: test\r\n\r\nGET /ws HTTP/1.1\r\nHost: "
        "test\r\nConnection: Upgrade\r\nUpgrade: "
        "websocket\r\nSec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    input += build_masked_client_frame(WebSocketOpcode::kText, "hel", false) +
             build_masked_client_frame(WebSocketOpcode::kPing, "p") +
             build_masked_client_frame(WebSocketOpcode::kContinuation, "lo");
    ASSERT_TRUE(send_all(fd, input));
    const auto deadline = zco::Deadline::after(std::chrono::seconds(1));
    std::string output = reader.until("0\r\n\r\n", deadline).value();
    output += reader.until("\r\n\r\n", deadline).value();
    output += recv_websocket_frame(reader, deadline).value();
    output += recv_websocket_frame(reader, deadline).value();
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
}
TEST(WebSocketServerIntegrationTest, LocalCloseWaitsForPeerWithBoundedTimeout) {
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
    auto server = make_server();
    register_websocket(*server, "/ws", std::move(callbacks), options);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    SocketReader reader(fd, 1);
    ASSERT_TRUE(send_all(
        fd, "GET /ws HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: "
            "websocket\r\nSec-WebSocket-Key: "
            "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    const auto begin = std::chrono::steady_clock::now();
    const auto output =
        reader.until_closed(zco::Deadline::after(std::chrono::seconds(1)))
            .value();
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
}

TEST(WebSocketServerIntegrationTest,
     ClientCloseDuringPartialFrameNotifiesOnceWithoutDeliveringMessage) {
    std::promise<void> close_notification;
    auto closed = close_notification.get_future();
    std::atomic<int> close_count{0}, messages{0};
    WebSocketCallbacks callbacks;
    callbacks.on_message = [&](const WebSocketConnection::ptr &, std::string &&,
                               WebSocketMessageType) { ++messages; };
    callbacks.on_close = [&](const WebSocketConnection::ptr &connection,
                             uint16_t, const std::string &) {
        EXPECT_EQ(connection->state(), WebSocketConnection::State::Closed);
        if (++close_count == 1)
            close_notification.set_value();
    };
    auto server = make_server();
    register_websocket(*server, "/ws", std::move(callbacks));
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    SocketReader reader(client.value().native_handle(), 1);
    ASSERT_TRUE(send_all(
        client.value().native_handle(),
        "GET /ws HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: "
        "websocket\r\nSec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    const auto handshake =
        reader.until("\r\n\r\n", zco::Deadline::after(std::chrono::seconds(1)))
            .value();
    ASSERT_NE(handshake.find("101 Switching Protocols"), std::string::npos);
    ASSERT_TRUE(send_all(client.value().native_handle(),
                         std::string("\x81\x85\x12\x34", 4)));
    ASSERT_TRUE(client.value().close());
    ASSERT_EQ(closed.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    server->stop();
    EXPECT_EQ(close_count.load(), 1);
    EXPECT_EQ(messages.load(), 0);
}

} // namespace zhttp
