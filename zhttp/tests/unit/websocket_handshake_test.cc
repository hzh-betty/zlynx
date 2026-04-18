#include "zhttp/http_request.h"
#include "zhttp/websocket.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>
#include <stdexcept>

using namespace zhttp;

namespace {

HttpRequest::ptr make_valid_websocket_request() {
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_version(HttpVersion::HTTP_1_1);
    request->set_header("Connection", "Upgrade");
    request->set_header("Upgrade", "websocket");
    request->set_header("Sec-WebSocket-Version", "13");
    request->set_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    return request;
}

std::string build_masked_client_frame(WebSocketOpcode opcode,
                                      const std::string &payload,
                                      bool fin = true) {
    std::string frame;
    const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                               static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(first));

    const uint8_t mask_key[4] = {0x21, 0x43, 0x65, 0x87};
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    frame.append(reinterpret_cast<const char *>(mask_key), sizeof(mask_key));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));
    }

    return frame;
}

} // namespace

TEST(WebSocketHandshakeTest, ComputesAcceptKeyFromRfcExample) {
    const std::string accept_key =
        compute_websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==");

    EXPECT_EQ(accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketHandshakeTest, RejectsInvalidHandshakeRequests) {
    std::string error;
    EXPECT_EQ(check_websocket_handshake_request(nullptr, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("null"), std::string::npos);

    auto request = make_valid_websocket_request();

    request->set_method(HttpMethod::POST);
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("GET"), std::string::npos);

    request->set_method(HttpMethod::GET);
    request->set_version(HttpVersion::HTTP_1_0);
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("HTTP/1.1"), std::string::npos);

    request->set_version(HttpVersion::HTTP_1_1);
    request->set_header("Connection", "keep-alive");
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("Connection"), std::string::npos);

    request->set_header("Connection", "keep-alive, upgrade");
    request->set_header("Upgrade", "h2c");
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("Upgrade"), std::string::npos);

    request->set_header("Upgrade", "websocket");
    request->set_header("Sec-WebSocket-Version", "12");
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kUnsupportedVersion);
    EXPECT_NE(error.find("Version"), std::string::npos);

    request->set_header("Sec-WebSocket-Version", "13");
    request->set_header("Sec-WebSocket-Key", "  ");
    EXPECT_EQ(check_websocket_handshake_request(request, &error),
              WebSocketHandshakeResult::kBadRequest);
    EXPECT_NE(error.find("Sec-WebSocket-Key"), std::string::npos);
}

TEST(WebSocketHandshakeTest, HandshakeValidationWorksWithoutErrorBuffer) {
    auto request = make_valid_websocket_request();
    request->set_header("Sec-WebSocket-Version", "12");
    EXPECT_EQ(check_websocket_handshake_request(request, nullptr),
              WebSocketHandshakeResult::kUnsupportedVersion);
}

TEST(WebSocketHandshakeTest, BuildsSwitchingProtocolsResponse) {
    auto request = make_valid_websocket_request();
    request->set_header("Connection", "keep-alive, Upgrade");

    std::string response;
    std::string error;
    const bool ok =
        build_websocket_handshake_response(request, &response, &error);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_NE(response.find("HTTP/1.1 101 Switching Protocols"),
              std::string::npos)
        << response;
    EXPECT_NE(response.find("Upgrade: websocket"), std::string::npos)
        << response;
    EXPECT_NE(response.find("Connection: Upgrade"), std::string::npos)
        << response;
    EXPECT_NE(
        response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
        std::string::npos)
        << response;
}

TEST(WebSocketHandshakeTest, BuildResponseRejectsNullOutputOrInvalidProtocol) {
    auto request = make_valid_websocket_request();
    std::string error;
    EXPECT_FALSE(build_websocket_handshake_response(request, nullptr, &error));
    EXPECT_NE(error.find("null"), std::string::npos);

    std::string response;
    EXPECT_FALSE(build_websocket_handshake_response(request, &response, &error,
                                                    "invalid protocol"));
    EXPECT_NE(error.find("Invalid selected subprotocol"), std::string::npos);
}

TEST(WebSocketHandshakeTest, BuildsResponseWithNegotiatedSubprotocol) {
    auto request = make_valid_websocket_request();
    request->set_header("Sec-WebSocket-Protocol", "chat, superchat");

    std::string response;
    std::string error;
    const bool ok = build_websocket_handshake_response(request, &response,
                                                       &error, "superchat");

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: superchat"),
              std::string::npos)
        << response;
}

TEST(WebSocketHandshakeTest, NegotiatesSubprotocolByServerPriority) {
    auto request = make_valid_websocket_request();
    request->set_header("Sec-WebSocket-Protocol", "json, msgpack");

    WebSocketOptions options;
    options.subprotocols = {"msgpack", "json"};

    std::string selected;
    std::string error;
    ASSERT_TRUE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_EQ(selected, "msgpack");
    EXPECT_TRUE(error.empty());
}

TEST(WebSocketHandshakeTest, SubprotocolNegotiationValidatesErrorPaths) {
    WebSocketOptions options;
    options.subprotocols = {"chat"};

    std::string selected;
    std::string error;
    auto request = make_valid_websocket_request();
    request->set_header("Sec-WebSocket-Protocol", "chat");

    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, nullptr, &error));
    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, &selected, nullptr));

    EXPECT_FALSE(
        negotiate_websocket_subprotocol(nullptr, options, &selected, &error));
    EXPECT_NE(error.find("null"), std::string::npos);

    request->set_header("Sec-WebSocket-Protocol", "chat, bad token");
    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_NE(error.find("Invalid"), std::string::npos);

    request->set_header("Sec-WebSocket-Protocol", "chat");
    options.subprotocols = {"bad token"};
    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_NE(error.find("Invalid server"), std::string::npos);

    options.subprotocols = {"protobuf"};
    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_NE(error.find("No compatible"), std::string::npos);

    request->set_header("Sec-WebSocket-Protocol", "");
    options.subprotocols = {"chat"};
    EXPECT_TRUE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_TRUE(selected.empty());

    request->set_header("Sec-WebSocket-Protocol", "chat");
    options.subprotocols.clear();
    EXPECT_TRUE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_TRUE(selected.empty());

    request->set_header("Sec-WebSocket-Protocol", std::string("chat,\x7f", 6));
    options.subprotocols = {"chat"};
    EXPECT_FALSE(
        negotiate_websocket_subprotocol(request, options, &selected, &error));
    EXPECT_NE(error.find("Invalid"), std::string::npos);
}

TEST(WebSocketHandshakeTest, ConnectionReportsDisconnectedForExpiredSocket) {
    WebSocketConnection connection(std::weak_ptr<znet::TcpConnection>{},
                                   "superchat");

    EXPECT_EQ(connection.selected_subprotocol(), "superchat");
    EXPECT_FALSE(connection.connected());
    EXPECT_EQ(connection.fd(), -1);
    EXPECT_FALSE(connection.send_text("hello"));
    EXPECT_FALSE(connection.send_binary("bin"));
    EXPECT_FALSE(connection.ping("p"));
    EXPECT_FALSE(connection.pong("q"));
    EXPECT_FALSE(connection.close(WebSocketCloseCode::kNormalClosure, "bye"));

    connection.mark_closed();
    EXPECT_FALSE(connection.connected());
}

TEST(WebSocketHandshakeTest, SessionOnOpenHandlesCallbacksAndExceptions) {
    {
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), WebSocketCallbacks{},
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");
        EXPECT_TRUE(session.on_open());
    }

    {
        int close_count = 0;
        int error_count = 0;
        uint16_t close_code = 0;
        std::string close_reason;
        std::string error_text;
        WebSocketCallbacks callbacks;
        callbacks.on_open = [](const WebSocketConnection::ptr &,
                               const HttpRequest::ptr &) {
            throw std::runtime_error("open boom");
        };
        callbacks.on_close = [&close_count, &close_code, &close_reason](
                                 const WebSocketConnection::ptr &, uint16_t code,
                                 const std::string &reason) {
            ++close_count;
            close_code = code;
            close_reason = reason;
        };
        callbacks.on_error = [&error_count, &error_text](
                                 const WebSocketConnection::ptr &,
                                 const std::string &text) {
            ++error_count;
            error_text = text;
        };

        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");
        EXPECT_FALSE(session.on_open());
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
        EXPECT_EQ(close_code,
                  static_cast<uint16_t>(WebSocketCloseCode::kInternalError));
        EXPECT_NE(close_reason.find("on_open callback failed"),
                  std::string::npos);
        EXPECT_NE(error_text.find("open boom"), std::string::npos);
    }

    {
        int close_count = 0;
        int error_count = 0;
        WebSocketCallbacks callbacks;
        callbacks.on_open = [](const WebSocketConnection::ptr &,
                               const HttpRequest::ptr &) { throw 42; };
        callbacks.on_close = [&close_count](const WebSocketConnection::ptr &,
                                            uint16_t, const std::string &) {
            ++close_count;
        };
        callbacks.on_error = [&error_count](const WebSocketConnection::ptr &,
                                            const std::string &) {
            ++error_count;
        };

        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");
        EXPECT_FALSE(session.on_open());
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
    }
}

TEST(WebSocketHandshakeTest, SessionOnMessageDeliversTextAndBinary) {
    std::vector<std::string> texts;
    std::vector<WebSocketMessageType> types;
    WebSocketCallbacks callbacks;
    callbacks.on_message = [&texts, &types](const WebSocketConnection::ptr &,
                                            std::string &&payload,
                                            WebSocketMessageType type) {
        texts.push_back(std::move(payload));
        types.push_back(type);
    };

    WebSocketSession session(
        nullptr, make_valid_websocket_request(), callbacks,
        WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

    znet::Buffer buffer;
    buffer.append(build_masked_client_frame(WebSocketOpcode::kText, "hello"));
    buffer.append(build_masked_client_frame(WebSocketOpcode::kBinary, "abc"));

    EXPECT_TRUE(session.on_message(&buffer));
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[0], "hello");
    EXPECT_EQ(texts[1], "abc");
    EXPECT_EQ(types[0], WebSocketMessageType::kText);
    EXPECT_EQ(types[1], WebSocketMessageType::kBinary);
}

TEST(WebSocketHandshakeTest, SessionOnMessageHandlesErrorBranches) {
    {
        int close_count = 0;
        int error_count = 0;
        std::string error_text;
        WebSocketCallbacks callbacks;
        callbacks.on_close = [&close_count](const WebSocketConnection::ptr &,
                                            uint16_t, const std::string &) {
            ++close_count;
        };
        callbacks.on_error = [&error_count, &error_text](
                                 const WebSocketConnection::ptr &,
                                 const std::string &text) {
            ++error_count;
            error_text = text;
        };
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

        znet::Buffer invalid;
        invalid.append(std::string("\x81\x01x", 3)); // unmasked client frame
        EXPECT_FALSE(session.on_message(&invalid));
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
        EXPECT_NE(error_text.find("masked"), std::string::npos);
    }

    {
        int close_count = 0;
        int error_count = 0;
        WebSocketCallbacks callbacks;
        callbacks.on_message = [](const WebSocketConnection::ptr &,
                                  std::string &&, WebSocketMessageType) {
            throw std::runtime_error("message boom");
        };
        callbacks.on_close = [&close_count](const WebSocketConnection::ptr &,
                                            uint16_t, const std::string &) {
            ++close_count;
        };
        callbacks.on_error = [&error_count](const WebSocketConnection::ptr &,
                                            const std::string &) {
            ++error_count;
        };
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

        znet::Buffer text;
        text.append(build_masked_client_frame(WebSocketOpcode::kText, "x"));
        EXPECT_FALSE(session.on_message(&text));
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
    }

    {
        int close_count = 0;
        int error_count = 0;
        WebSocketCallbacks callbacks;
        callbacks.on_message = [](const WebSocketConnection::ptr &,
                                  std::string &&, WebSocketMessageType) {
            throw 123;
        };
        callbacks.on_close = [&close_count](const WebSocketConnection::ptr &,
                                            uint16_t, const std::string &) {
            ++close_count;
        };
        callbacks.on_error = [&error_count](const WebSocketConnection::ptr &,
                                            const std::string &) {
            ++error_count;
        };
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

        znet::Buffer binary;
        binary.append(build_masked_client_frame(WebSocketOpcode::kBinary, "x"));
        EXPECT_FALSE(session.on_message(&binary));
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
    }
}

TEST(WebSocketHandshakeTest, SessionPingFailureAndCloseNotificationAreHandled) {
    {
        int close_count = 0;
        int error_count = 0;
        WebSocketCallbacks callbacks;
        callbacks.on_close = [&close_count](const WebSocketConnection::ptr &,
                                            uint16_t, const std::string &) {
            ++close_count;
        };
        callbacks.on_error = [&error_count](const WebSocketConnection::ptr &,
                                            const std::string &) {
            ++error_count;
        };
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

        znet::Buffer ping;
        ping.append(build_masked_client_frame(WebSocketOpcode::kPing, "hb"));
        EXPECT_FALSE(session.on_message(&ping));
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(error_count, 1);
    }

    {
        int close_count = 0;
        uint16_t close_code = 0;
        std::string close_reason;
        WebSocketCallbacks callbacks;
        callbacks.on_close = [&close_count, &close_code, &close_reason](
                                 const WebSocketConnection::ptr &, uint16_t code,
                                 const std::string &reason) {
            ++close_count;
            close_code = code;
            close_reason = reason;
        };
        WebSocketSession session(
            nullptr, make_valid_websocket_request(), callbacks,
            WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");

        std::string close_payload;
        close_payload.push_back(static_cast<char>(0x03));
        close_payload.push_back(static_cast<char>(0xE8));
        close_payload.append("bye");

        znet::Buffer close_frame;
        close_frame.append(
            build_masked_client_frame(WebSocketOpcode::kClose, close_payload));
        EXPECT_FALSE(session.on_message(&close_frame));
        EXPECT_EQ(close_count, 1);
        EXPECT_EQ(close_code,
                  static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure));
        EXPECT_EQ(close_reason, "bye");

        session.on_close();
        session.on_close();
        EXPECT_EQ(close_count, 1);
    }
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
