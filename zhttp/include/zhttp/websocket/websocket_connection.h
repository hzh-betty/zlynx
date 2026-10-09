#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_CONNECTION_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_CONNECTION_H_

#include "zhttp/websocket/websocket_types.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
namespace znet {
class Connection;
}
namespace zhttp {
/**
 * 已升级连接的 WebSocket 发送与关闭接口。
 *
 * 发送、ping/pong 和主动关闭须在连接回调内调用；不提供跨线程控制队列。
 * 仅弱引用底层 TCP 连接，不延长网络连接寿命。
 */
class WebSocketConnection {
  public:
    using ptr = std::shared_ptr<WebSocketConnection>;
    /** 连接状态：打开、等待关闭握手、已关闭。 */
    enum class State { Open, Closing, Closed };
    /** 返回原子读取的连接状态。 */
    State state() const { return state_.load(std::memory_order_acquire); }

    /** 绑定底层连接、协商的子协议和关闭握手超时（毫秒）。 */
    WebSocketConnection(std::weak_ptr<znet::Connection> connection,
                        std::string selected_subprotocol,
                        uint32_t close_timeout_ms = 5000);

    // 发送与主动关闭应在连接回调内完成，不提供跨线程控制队列。
    /** 发送文本帧；调用方保证 UTF-8 合法，载荷超过 1 MiB 或发送失败返回 false。 */
    bool send_text(const std::string &message);
    /** 发送二进制帧；载荷超过 1 MiB、连接关闭或发送失败返回 false。 */
    bool send_binary(const std::string &payload);
    /** 发送 ping 控制帧；载荷最多 125 字节，失败返回 false。 */
    bool ping(const std::string &payload = "");
    /** 发送 pong 控制帧；载荷最多 125 字节，失败返回 false。 */
    bool pong(const std::string &payload = "");
    /**
     * 发起关闭握手。
     *
     * @param code 合法的关闭状态码，默认正常关闭。
     * @param reason UTF-8 原因文本；超出 123 字节时按完整字符边界截断。
     * @return 成功或已经进入关闭状态返回 true；关闭码、编码或发送失败返回 false。
     */
    bool close(WebSocketCloseCode code = WebSocketCloseCode::kNormalClosure,
               const std::string &reason = "");

    /** 返回 WebSocket 为 Open 且底层连接仍可用。 */
    bool connected() const;
    /** 返回底层文件描述符；底层连接已释放时返回 -1。 */
    int fd() const;

    /** 返回协商后的子协议，未选择时为空。 */
    const std::string &selected_subprotocol() const {
        return selected_subprotocol_;
    }

  private:
    friend class WebSocketProtocolHandler;
    void mark_closed();
    bool accept_close(WebSocketCloseCode code, const std::string &reason);
    bool write_frame(WebSocketOpcode opcode, const std::string &payload,
                       bool fin = true, bool peer_close = false);
    bool send_frame(WebSocketOpcode opcode, const std::string &payload,
                    bool fin = true);

  private:
    std::weak_ptr<znet::Connection> connection_;
    std::string selected_subprotocol_;
    std::atomic<State> state_{State::Open};
    uint32_t close_timeout_ms_;
};

} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_WEBSOCKET_CONNECTION_H_
