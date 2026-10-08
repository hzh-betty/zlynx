#pragma once
#include "zhttp/websocket/websocket_types.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
namespace znet {
class TcpConnection;
}
namespace zhttp {
class WebSocketConnection {
  public:
    using ptr = std::shared_ptr<WebSocketConnection>;
    enum class State { Open, Closing, Closed };
    State state() const { return state_.load(std::memory_order_acquire); }

    WebSocketConnection(std::weak_ptr<znet::TcpConnection> connection,
                        std::string selected_subprotocol,
                        uint32_t close_timeout_ms = 5000);

    // 发送与主动关闭应在连接回调内完成，不提供跨线程控制队列。
    bool send_text(const std::string &message);
    bool send_binary(const std::string &payload);
    bool ping(const std::string &payload = "");
    bool pong(const std::string &payload = "");
    bool close(WebSocketCloseCode code = WebSocketCloseCode::kNormalClosure,
               const std::string &reason = "");

    bool connected() const;
    int fd() const;

    const std::string &selected_subprotocol() const {
        return selected_subprotocol_;
    }

    void mark_closed();

  private:
    friend class WebSocketProtocolHandler;
    bool accept_close(WebSocketCloseCode code, const std::string &reason);
    bool write_frame(WebSocketOpcode opcode, const std::string &payload,
                       bool fin = true, bool peer_close = false);
    bool send_frame(WebSocketOpcode opcode, const std::string &payload,
                    bool fin = true);

  private:
    std::weak_ptr<znet::TcpConnection> connection_;
    std::string selected_subprotocol_;
    std::atomic<State> state_{State::Open};
    uint32_t close_timeout_ms_;
};

} // namespace zhttp
