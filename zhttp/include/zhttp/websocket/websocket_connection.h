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
 * @brief 已升级连接的 WebSocket 发送与关闭接口。
 *
 * 发送、ping/pong 和主动关闭须在连接回调内调用；不提供跨线程控制队列。
 * 仅弱引用底层 TCP 连接，不延长网络连接寿命。
 */
class WebSocketConnection {
  public:
    using ptr = std::shared_ptr<WebSocketConnection>;

    /*
    * WebSocketConnection::State 状态转换图
    *
    *   创建对象
    *       |
    *       v
    *   +------+  主动 close() / 接收对端 Close  +---------+
    *   | Open |------------------------------->| Closing |
    *   +------+                                +---------+
    *       |                                        |
    *       | 底层连接关闭通知                        | 底层连接关闭通知
    *       v                                        v
    *   +------------------------------------------------+
    *   |                     Closed                     |
    *   +------------------------------------------------+
    *
    * 状态说明：
    *   Open    : 允许发送数据帧和控制帧
    *   Closing : 已进入关闭流程，等待关闭握手或底层连接结束
    *   Closed  : 已收到底层关闭通知，不再发送
    *
    * 常规转换：
    *   创建对象 -> Open
    *   Open     -> Closing : close() 参数合法，开始主动关闭
    *   Open     -> Closing : accept_close() 接收对端关闭帧并尝试回复
    *   Closing  -> Closed  : on_closed() 调用 mark_closed()
    *   Open     -> Closed  : 底层连接直接关闭，on_closed() 调用 mark_closed()
    *
    * 特殊路径：
    *   Closing -> Closing : 接收对端 Close，调用 shutdown()
    *   Closing -> Closing : 重复调用参数合法的 close()，直接返回 true
    *   Closed  -> Closed  : 重复关闭通知；或调用参数合法的 close()
    *   Open    -> Open    : close() 的关闭码或 UTF-8 原因文本非法
    *
    * 注意：
    *   close() 先设置 Closing，再发送关闭帧。
    *   关闭帧发送失败不会把状态恢复为 Open。
    *   Closing 下普通发送入口只允许 Pong。
    *   主动关闭发送成功后设置读取期限，等待对端关闭响应。
    *   回复对端关闭帧成功后调用 shutdown()。
    *   Closed 由 mark_closed() 设置，不由 close() 直接设置。
    *   业务 on_close 通知与 Closed 状态并不同时发生：
    *   收到对端关闭帧或发生协议错误时，可能先通知业务，稍后才进入 Closed。
    */
    enum class State { Open, Closing, Closed };

    /**
     * @brief 返回原子读取的连接状态。
     */
    State state() const { return state_.load(std::memory_order_acquire); }

    /**
     * @brief 绑定底层连接、协商的子协议和关闭握手超时（毫秒）。
     */
    WebSocketConnection(std::weak_ptr<znet::Connection> connection,
                        std::string selected_subprotocol,
                        uint32_t close_timeout_ms = 5000);

    // 发送与主动关闭应在连接回调内完成，不提供跨线程控制队列。
    /**
     * @brief 发送文本帧；调用方保证 UTF-8 合法，载荷超过 1 MiB 或发送失败返回
     * false。
     */
    bool send_text(const std::string &message);

    /**
     * @brief 发送二进制帧；载荷超过 1 MiB、连接关闭或发送失败返回 false。
     */
    bool send_binary(const std::string &payload);

    /**
     * @brief 发送 ping 控制帧；载荷最多 125 字节，失败返回 false。
     */
    bool ping(const std::string &payload = "");

    /**
     * @brief 发送 pong 控制帧；载荷最多 125 字节，失败返回 false。
     */
    bool pong(const std::string &payload = "");

    /**
     * @brief 发起关闭握手。
     *
     * @param code 合法的关闭状态码，默认正常关闭。
     * @param reason UTF-8 原因文本；超出 123 字节时按完整字符边界截断。
     * @return 成功或已经进入关闭状态返回 true；关闭码、编码或发送失败返回
     * false。
     */
    bool close(WebSocketCloseCode code = WebSocketCloseCode::kNormalClosure,
               const std::string &reason = "");

    /**
     * @brief 返回 WebSocket 为 Open 且底层连接仍可用。
     */
    bool connected() const;

    /**
     * @brief 返回底层文件描述符；底层连接已释放时返回 -1。
     */
    int fd() const;

    /**
     * @brief 返回协商后的子协议，未选择时为空。
     */
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

#endif
