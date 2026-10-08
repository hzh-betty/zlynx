#include "zhttp/websocket/websocket_connection.h"
#include "zhttp/websocket/websocket_frame.h"
#include "zhttp/writer/http_response_writer.h"
#include "znet/tcp_connection.h"
namespace zhttp {
namespace {
std::string build_close_payload(const WebSocketCloseCode close_code,
                                const std::string &reason) {
    std::string payload;
    payload.reserve(2 + reason.size());

    const uint16_t code = static_cast<uint16_t>(close_code);
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));

    // WebSocket 关闭帧控制载荷最大 125 字节，扣除 2 字节 code 后 reason 最多
    // 123。
    constexpr size_t kMaxCloseReasonSize = 123;
    if (reason.size() <= kMaxCloseReasonSize) {
        payload.append(reason);
    } else {
        std::string shortened = reason.substr(0, kMaxCloseReasonSize);
        while (!valid_websocket_utf8(shortened))
            shortened.pop_back();
        payload.append(shortened);
    }

    return payload;
}

} // namespace
WebSocketConnection::WebSocketConnection(
    std::weak_ptr<znet::TcpConnection> connection,
    std::string selected_subprotocol, uint32_t close_timeout_ms)
    : connection_(std::move(connection)),
      selected_subprotocol_(std::move(selected_subprotocol)),
      close_timeout_ms_(close_timeout_ms ? close_timeout_ms : 5000) {}

bool WebSocketConnection::send_text(const std::string &message) {
    return send_frame(WebSocketOpcode::kText, message, true);
}

bool WebSocketConnection::send_binary(const std::string &payload) {
    return send_frame(WebSocketOpcode::kBinary, payload, true);
}

bool WebSocketConnection::ping(const std::string &payload) {
    return send_frame(WebSocketOpcode::kPing, payload, true);
}

bool WebSocketConnection::pong(const std::string &payload) {
    return send_frame(WebSocketOpcode::kPong, payload, true);
}

bool WebSocketConnection::close(WebSocketCloseCode code,
                                const std::string &reason) {
    if (!valid_websocket_close_code(static_cast<uint16_t>(code)) ||
        !valid_websocket_utf8(reason))
        return false;
    if (state() != State::Open)
        return true;
    state_.store(State::Closing, std::memory_order_release);
    const bool accepted = write_frame(WebSocketOpcode::kClose,
                                        build_close_payload(code, reason));
    if (!accepted)
        if (auto conn = connection_.lock())
            conn->close();
    return accepted;
}
bool WebSocketConnection::accept_close(WebSocketCloseCode code,
                                       const std::string &reason) {
    auto conn = connection_.lock();
    if (!conn || state() == State::Closed)
        return false;
    const bool was_open = state() == State::Open;
    state_.store(State::Closing, std::memory_order_release);
    if (!was_open) {
        conn->shutdown();
        return true;
    }
    const bool accepted = write_frame(WebSocketOpcode::kClose,
                                      build_close_payload(code, reason), true,
                                      true);
    if (!accepted)
        conn->close();
    return accepted;
}

bool WebSocketConnection::connected() const {
    if (state() != State::Open) {
        return false;
    }

    auto conn = connection_.lock();
    return conn && conn->connected();
}

int WebSocketConnection::fd() const {
    auto conn = connection_.lock();
    return conn ? conn->fd() : -1;
}

void WebSocketConnection::mark_closed() {
    state_.store(State::Closed, std::memory_order_release);
}

bool WebSocketConnection::send_frame(WebSocketOpcode opcode,
                                     const std::string &payload, bool fin) {
    if (state() == State::Closed ||
        (state() == State::Closing && opcode != WebSocketOpcode::kPong))
        return false;
    return write_frame(opcode, payload, fin);
}
bool WebSocketConnection::write_frame(WebSocketOpcode opcode,
                                        const std::string &payload, bool fin,
                                        bool peer_close) {
    auto conn = connection_.lock();
    if (!conn || !conn->connected() || payload.size() > 1024 * 1024)
        return false;
    std::string frame;
    if (!build_websocket_frame(opcode, payload, &frame, fin))
        return false;
    // 在 WebSocket 连接回调中直接发送，沿用 znet 的既有发送与关闭接口。
    if (!send_all_or_fail(conn, frame.data(), frame.size())) {
        conn->close();
        return false;
    }
    if (opcode == WebSocketOpcode::kClose) {
        if (peer_close)
            conn->shutdown();
        else
            conn->set_read_deadline(close_timeout_ms_);
    }
    return true;
}
} // namespace zhttp
