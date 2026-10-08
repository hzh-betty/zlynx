#include "zhttp/protocol/websocket_protocol_handler.h"
#include "znet/buffer.h"
#include "znet/tcp_connection.h"
namespace zhttp {
WebSocketProtocolHandler::WebSocketProtocolHandler(
    std::shared_ptr<znet::TcpConnection> connection,
    std::shared_ptr<const HttpRequest> request, WebSocketCallbacks callbacks,
    WebSocketOptions options, std::string selected_subprotocol)
    : connection_(std::make_shared<WebSocketConnection>(
          std::move(connection), std::move(selected_subprotocol),
          options.close_timeout_ms)),
      request_(std::move(request)), callbacks_(std::move(callbacks)),
      max_message_size_(options.max_message_size == 0
                            ? kDefaultWebSocketMaxMessageSize
                            : options.max_message_size),
      assembler_(max_message_size_) {}

bool WebSocketProtocolHandler::on_open() {
    if (!callbacks_.on_open) {
        request_.reset();
        return true;
    }

    try {
        auto request = std::move(request_);
        callbacks_.on_open(connection_, request);
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

void WebSocketProtocolHandler::on_data(
    const std::shared_ptr<znet::TcpConnection> &, znet::Buffer &buffer) {
    if (close_notified_)
        return;
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code =
        static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
    std::string error;

    while (buffer.readable_bytes() > 0) {
        std::vector<WebSocketFrameEvent> frames;
        events.clear();
        if (!parse_websocket_frame(&buffer, &frames, &close_code, &error,
                                   max_message_size_) ||
            (!frames.empty() &&
             !assembler_.assemble(std::move(frames.front()), events, close_code,
                                  error))) {
            notify_error(error);
            close_frame_sent_ = true;
            connection_->close(static_cast<WebSocketCloseCode>(close_code),
                               error);
            notify_close(close_code, error);
            return;
        }
        if (frames.empty())
            break;
        for (auto &event : events) {
            if (event.opcode == WebSocketOpcode::kText ||
                event.opcode == WebSocketOpcode::kBinary) {
                // Text/Binary 只在“完整消息”粒度回调给业务层。
                if (!callbacks_.on_message ||
                    connection_->state() != WebSocketConnection::State::Open) {
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
                    notify_close(static_cast<uint16_t>(
                                     WebSocketCloseCode::kInternalError),
                                 "on_message callback failed");
                    return;
                } catch (...) {
                    notify_error("WebSocket on_message callback threw unknown "
                                 "exception");
                    close_frame_sent_ = true;
                    connection_->close(WebSocketCloseCode::kInternalError,
                                       "on_message callback failed");
                    notify_close(static_cast<uint16_t>(
                                     WebSocketCloseCode::kInternalError),
                                 "on_message callback failed");
                    return;
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
                    notify_close(static_cast<uint16_t>(
                                     WebSocketCloseCode::kInternalError),
                                 "pong failed");
                    return;
                }
                continue;
            }

            if (event.opcode == WebSocketOpcode::kPong) {
                // 当前实现不维护心跳状态，仅消费该控制帧。
                continue;
            }

            if (event.opcode == WebSocketOpcode::kClose) {
                close_frame_sent_ = true;
                connection_->accept_close(
                    static_cast<WebSocketCloseCode>(event.close_code),
                    event.payload);
                notify_close(event.close_code, event.payload);
                return;
            }
        }
    }
}

void WebSocketProtocolHandler::on_closed() {
    if (connection_) {
        connection_->mark_closed();
    }
    notify_close(static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure), "");
}

void WebSocketProtocolHandler::notify_error(const std::string &error) {
    if (callbacks_.on_error) {
        try {
            callbacks_.on_error(connection_, error);
        } catch (...) {
        }
    }
}

void WebSocketProtocolHandler::notify_close(const uint16_t close_code,
                                            const std::string &reason) {
    // on_close 只允许触发一次，避免业务层重复清理。
    if (close_notified_) {
        return;
    }
    close_notified_ = true;

    if (callbacks_.on_close) {
        try {
            callbacks_.on_close(connection_, close_code, reason);
        } catch (...) {
        }
    }
}

} // namespace zhttp
