#include "zhttp/websocket.h"

#include "zhttp/http_common.h"

#include <cctype>
#include <sstream>
#include <utility>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace zhttp {
namespace {

constexpr char kWebSocketAcceptGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool header_contains_token(const std::string &header_value,
                           const std::string &target) {
    // Connection 头可能包含逗号分隔 token（如 keep-alive, Upgrade）。
    // 这里按 token 语义匹配，避免 substring 误判。
    const auto tokens = split_string(header_value, ',');
    for (std::string token : tokens) {
        trim(token);
        if (to_lower(token) == target) {
            return true;
        }
    }
    return false;
}

bool is_http_token_char(const unsigned char ch) {
    if (ch <= 0x1F || ch >= 0x7F) {
        return false;
    }

    switch (ch) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t':
        return false;
    default:
        return true;
    }
}

bool is_valid_subprotocol_token(const std::string &token) {
    if (token.empty()) {
        return false;
    }

    for (const unsigned char ch : token) {
        if (!is_http_token_char(ch)) {
            return false;
        }
    }

    return true;
}

bool parse_subprotocol_header(const std::string &header_value,
                              std::vector<std::string> *protocols,
                              std::string *error) {
    if (!protocols || !error) {
        return false;
    }

    protocols->clear();
    error->clear();

    if (header_value.empty()) {
        return true;
    }

    // Sec-WebSocket-Protocol 按 RFC 定义为 token 列表，不允许任意字符。
    const auto tokens = split_string(header_value, ',');
    for (std::string token : tokens) {
        trim(token);
        if (!is_valid_subprotocol_token(token)) {
            *error = "Invalid Sec-WebSocket-Protocol token";
            return false;
        }
        protocols->push_back(std::move(token));
    }

    return true;
}

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
        payload.append(reason.data(), kMaxCloseReasonSize);
    }

    return payload;
}

} // namespace

WebSocketConnection::WebSocketConnection(
    std::weak_ptr<znet::TcpConnection> connection,
    std::string selected_subprotocol)
    : connection_(std::move(connection)),
      selected_subprotocol_(std::move(selected_subprotocol)), closed_(false) {}

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

bool WebSocketConnection::close(const WebSocketCloseCode code,
                                const std::string &reason) {
    const bool already_closed =
        closed_.exchange(true, std::memory_order_acq_rel);
    auto conn = connection_.lock();
    if (!conn) {
        return false;
    }

    bool send_ok = true;
    if (!already_closed) {
        std::string frame;
        const std::string close_payload = build_close_payload(code, reason);
        if (!build_websocket_frame(WebSocketOpcode::kClose, close_payload,
                                   &frame, true)) {
            send_ok = false;
        } else if (conn->send(frame.data(), frame.size()) < 0) {
            send_ok = false;
        }
    }

    // 无论 Close 帧发送是否成功，都进入 shutdown，避免连接卡在半关闭状态。
    conn->shutdown();
    return send_ok;
}

bool WebSocketConnection::connected() const {
    if (closed_.load(std::memory_order_acquire)) {
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
    closed_.store(true, std::memory_order_release);
}

bool WebSocketConnection::send_frame(const WebSocketOpcode opcode,
                                     const std::string &payload,
                                     const bool fin) {
    if (closed_.load(std::memory_order_acquire)) {
        return false;
    }

    auto conn = connection_.lock();
    if (!conn || !conn->connected()) {
        return false;
    }

    std::string frame;
    if (!build_websocket_frame(opcode, payload, &frame, fin)) {
        return false;
    }

    return conn->send(frame.data(), frame.size()) >= 0;
}

WebSocketHandshakeResult
check_websocket_handshake_request(const HttpRequest::ptr &request,
                                  std::string *error) {
    if (!request) {
        if (error) {
            *error = "Request is null";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (request->method() != HttpMethod::GET) {
        if (error) {
            *error = "WebSocket upgrade only supports GET";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (request->version() != HttpVersion::HTTP_1_1) {
        if (error) {
            *error = "WebSocket upgrade requires HTTP/1.1";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    const std::string connection_header = request->header("Connection");
    if (!header_contains_token(connection_header, "upgrade")) {
        if (error) {
            *error = "Missing Connection: Upgrade";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    std::string upgrade_header = request->header("Upgrade");
    trim(upgrade_header);
    if (to_lower(upgrade_header) != "websocket") {
        if (error) {
            *error = "Missing Upgrade: websocket";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    std::string version = request->header("Sec-WebSocket-Version");
    trim(version);
    if (version != "13") {
        if (error) {
            *error = "Sec-WebSocket-Version must be 13";
        }
        return WebSocketHandshakeResult::kUnsupportedVersion;
    }

    std::string key = request->header("Sec-WebSocket-Key");
    trim(key);
    if (key.empty()) {
        if (error) {
            *error = "Missing Sec-WebSocket-Key";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (error) {
        error->clear();
    }
    return WebSocketHandshakeResult::kOk;
}

std::string compute_websocket_accept_key(const std::string &client_key) {
    const std::string challenge = client_key + kWebSocketAcceptGuid;

    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(challenge.data()),
         challenge.size(), digest);

    unsigned char encoded[((SHA_DIGEST_LENGTH + 2) / 3) * 4 + 1];
    const int encoded_size =
        EVP_EncodeBlock(encoded, digest, SHA_DIGEST_LENGTH);
    if (encoded_size <= 0) {
        return "";
    }

    return std::string(reinterpret_cast<char *>(encoded),
                       static_cast<size_t>(encoded_size));
}

bool negotiate_websocket_subprotocol(const HttpRequest::ptr &request,
                                     const WebSocketOptions &options,
                                     std::string *selected_subprotocol,
                                     std::string *error) {
    if (!selected_subprotocol || !error) {
        return false;
    }

    selected_subprotocol->clear();
    error->clear();

    if (!request) {
        *error = "Request is null";
        return false;
    }

    std::vector<std::string> client_protocols;
    if (!parse_subprotocol_header(request->header("Sec-WebSocket-Protocol"),
                                  &client_protocols, error)) {
        return false;
    }

    if (options.subprotocols.empty() || client_protocols.empty()) {
        // 任一方未声明子协议时，握手可继续，但不返回 Sec-WebSocket-Protocol。
        return true;
    }

    // 服务器按自身优先级选择第一个命中的子协议，保持行为可预测。
    for (const std::string &server_protocol : options.subprotocols) {
        if (!is_valid_subprotocol_token(server_protocol)) {
            *error = "Invalid server WebSocket subprotocol configuration";
            return false;
        }

        for (const std::string &client_protocol : client_protocols) {
            if (client_protocol == server_protocol) {
                *selected_subprotocol = server_protocol;
                return true;
            }
        }
    }

    *error = "No compatible Sec-WebSocket-Protocol";
    return false;
}

bool build_websocket_handshake_response(
    const HttpRequest::ptr &request, std::string *response, std::string *error,
    const std::string &selected_subprotocol) {
    if (!response) {
        if (error) {
            *error = "Response output is null";
        }
        return false;
    }

    const WebSocketHandshakeResult result =
        check_websocket_handshake_request(request, error);
    if (result != WebSocketHandshakeResult::kOk) {
        return false;
    }

    if (!selected_subprotocol.empty() &&
        !is_valid_subprotocol_token(selected_subprotocol)) {
        if (error) {
            *error = "Invalid selected subprotocol";
        }
        return false;
    }

    std::string key = request->header("Sec-WebSocket-Key");
    trim(key);
    const std::string accept_key = compute_websocket_accept_key(key);
    if (accept_key.empty()) {
        if (error) {
            *error = "Failed to compute Sec-WebSocket-Accept";
        }
        return false;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    if (!selected_subprotocol.empty()) {
        oss << "Sec-WebSocket-Protocol: " << selected_subprotocol << "\r\n";
    }
    oss << "\r\n";

    *response = oss.str();
    if (error) {
        error->clear();
    }
    return true;
}

WebSocketSession::WebSocketSession(
    std::shared_ptr<znet::TcpConnection> connection, HttpRequest::ptr request,
    WebSocketCallbacks callbacks, WebSocketOptions options,
    std::string selected_subprotocol)
    : connection_(std::make_shared<WebSocketConnection>(
          std::move(connection), std::move(selected_subprotocol))),
      request_(std::move(request)), callbacks_(std::move(callbacks)),
      parser_(options.max_message_size == 0 ? kDefaultWebSocketMaxMessageSize
                                            : options.max_message_size) {}

bool WebSocketSession::on_open() {
    if (!callbacks_.on_open) {
        return true;
    }

    try {
        callbacks_.on_open(connection_, request_);
        return true;
    } catch (const std::exception &ex) {
        notify_error(std::string("WebSocket on_open callback threw: ") +
                     ex.what());
        connection_->close(WebSocketCloseCode::kInternalError,
                           "on_open callback failed");
        notify_close(static_cast<uint16_t>(WebSocketCloseCode::kInternalError),
                     "on_open callback failed");
        return false;
    } catch (...) {
        notify_error("WebSocket on_open callback threw unknown exception");
        connection_->close(WebSocketCloseCode::kInternalError,
                           "on_open callback failed");
        notify_close(static_cast<uint16_t>(WebSocketCloseCode::kInternalError),
                     "on_open callback failed");
        return false;
    }
}

bool WebSocketSession::on_message(znet::Buffer *buffer) {
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code =
        static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
    std::string error;

    if (!parser_.parse(buffer, &events, &close_code, &error)) {
        notify_error(error);
        close_frame_sent_ = true;
        connection_->close(static_cast<WebSocketCloseCode>(close_code), error);
        notify_close(close_code, error);
        return false;
    }

    for (auto &event : events) {
        if (event.opcode == WebSocketOpcode::kText ||
            event.opcode == WebSocketOpcode::kBinary) {
            // Text/Binary 只在“完整消息”粒度回调给业务层。
            if (!callbacks_.on_message) {
                continue;
            }

            try {
                const WebSocketMessageType message_type =
                    event.opcode == WebSocketOpcode::kText
                        ? WebSocketMessageType::kText
                        : WebSocketMessageType::kBinary;
                callbacks_.on_message(connection_, std::move(event.payload),
                                      message_type);
            } catch (const std::exception &ex) {
                notify_error(
                    std::string("WebSocket on_message callback threw: ") +
                    ex.what());
                close_frame_sent_ = true;
                connection_->close(WebSocketCloseCode::kInternalError,
                                   "on_message callback failed");
                notify_close(
                    static_cast<uint16_t>(WebSocketCloseCode::kInternalError),
                    "on_message callback failed");
                return false;
            } catch (...) {
                notify_error(
                    "WebSocket on_message callback threw unknown exception");
                close_frame_sent_ = true;
                connection_->close(WebSocketCloseCode::kInternalError,
                                   "on_message callback failed");
                notify_close(
                    static_cast<uint16_t>(WebSocketCloseCode::kInternalError),
                    "on_message callback failed");
                return false;
            }
            continue;
        }

        if (event.opcode == WebSocketOpcode::kPing) {
            // Ping 必须尽快回 Pong；失败时视为连接不可用并走关闭路径。
            if (!connection_->pong(event.payload)) {
                notify_error("Failed to send WebSocket pong frame");
                close_frame_sent_ = true;
                connection_->close(WebSocketCloseCode::kInternalError,
                                   "pong failed");
                notify_close(
                    static_cast<uint16_t>(WebSocketCloseCode::kInternalError),
                    "pong failed");
                return false;
            }
            continue;
        }

        if (event.opcode == WebSocketOpcode::kPong) {
            // 当前实现不维护心跳状态，仅消费该控制帧。
            continue;
        }

        if (event.opcode == WebSocketOpcode::kClose) {
            if (!close_frame_sent_) {
                // 对端主动 Close 时，按其关闭码回送 Close 完成握手收尾。
                close_frame_sent_ = true;
                connection_->close(
                    static_cast<WebSocketCloseCode>(event.close_code),
                    event.payload);
            }
            notify_close(event.close_code, event.payload);
            return false;
        }
    }

    return true;
}

void WebSocketSession::on_close() {
    if (connection_) {
        connection_->mark_closed();
    }
    notify_close(static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure), "");
}

void WebSocketSession::notify_error(const std::string &error) {
    if (callbacks_.on_error) {
        callbacks_.on_error(connection_, error);
    }
}

void WebSocketSession::notify_close(const uint16_t close_code,
                                    const std::string &reason) {
    // on_close 只允许触发一次，避免业务层重复清理。
    if (close_notified_) {
        return;
    }
    close_notified_ = true;

    if (callbacks_.on_close) {
        callbacks_.on_close(connection_, close_code, reason);
    }
}

} // namespace zhttp
