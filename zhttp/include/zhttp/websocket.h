#ifndef ZHTTP_WEBSOCKET_H_
#define ZHTTP_WEBSOCKET_H_

#include "zhttp/websocket_frame.h"
#include "http_request.h"
#include "znet/tcp_connection.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zhttp {

/**
 * @brief WebSocket 会话参数
 * @details
 * - max_message_size 用于限制单条业务消息（包含分片重组后的完整消息）最大长度。
 * - subprotocols 表示服务端支持的子协议列表，顺序即服务端优先级。
 */
struct WebSocketOptions {
    size_t max_message_size = kDefaultWebSocketMaxMessageSize;
    std::vector<std::string> subprotocols;
};

/**
 * @brief WebSocket 发送封装
 * @details
 * 对业务层暴露“文本/二进制/Ping/Pong/Close”发送接口，内部完成帧编码并委托
 * TcpConnection 写出。该类仅弱引用连接，避免与会话形成循环引用。
 */
class WebSocketConnection {
  public:
    using ptr = std::shared_ptr<WebSocketConnection>;

    WebSocketConnection(std::weak_ptr<znet::TcpConnection> connection,
                        std::string selected_subprotocol);

    bool send_text(const std::string &message);
    bool send_binary(const std::string &payload);
    bool ping(const std::string &payload = "");
    bool pong(const std::string &payload = "");
    bool close(WebSocketCloseCode code = WebSocketCloseCode::kNormalClosure,
               const std::string &reason = "");

    bool connected() const;
    int fd() const;

    /**
     * @brief 获取握手阶段协商出的子协议
     * @note 若未协商子协议，则返回空字符串。
     */
    const std::string &selected_subprotocol() const {
        return selected_subprotocol_;
    }

    void mark_closed();

  private:
    bool send_frame(WebSocketOpcode opcode, const std::string &payload,
                    bool fin = true);

  private:
    std::weak_ptr<znet::TcpConnection> connection_;
    std::string selected_subprotocol_;
    std::atomic<bool> closed_;
};

using WebSocketOpenCallback = std::function<void(
    const WebSocketConnection::ptr &, const HttpRequest::ptr &)>;

using WebSocketMessageCallback = std::function<void(
    const WebSocketConnection::ptr &, std::string &&, WebSocketMessageType)>;

using WebSocketCloseCallback = std::function<void(
    const WebSocketConnection::ptr &, uint16_t, const std::string &)>;

using WebSocketErrorCallback =
    std::function<void(const WebSocketConnection::ptr &, const std::string &)>;

/**
 * @brief WebSocket 生命周期回调集合
 * @details
 * - on_open: 握手成功并注册会话后触发。
 * - on_message: 收到完整业务消息后触发（文本/二进制）。
 * - on_close: 会话关闭时触发，可能由对端 Close、协议错误或内部错误导致。
 * - on_error: 非正常处理路径的错误通知。
 */
struct WebSocketCallbacks {
    WebSocketOpenCallback on_open;
    WebSocketMessageCallback on_message;
    WebSocketCloseCallback on_close;
    WebSocketErrorCallback on_error;
};

enum class WebSocketHandshakeResult {
    kOk,
    kBadRequest,
    kUnsupportedVersion,
};

/**
 * @brief 校验 WebSocket 升级请求基础合法性
 * @return 校验结果；失败时会通过 error 返回可读原因。
 */
WebSocketHandshakeResult
check_websocket_handshake_request(const HttpRequest::ptr &request,
                                  std::string *error);

/**
 * @brief 计算握手响应用的 Sec-WebSocket-Accept
 */
std::string compute_websocket_accept_key(const std::string &client_key);

/**
 * @brief 协商 WebSocket 子协议
 * @details
 * 按服务端 options.subprotocols 的顺序选择首个与客户端请求交集中的协议。
 * 若双方任一方未声明子协议，视为无需协商并返回成功。
 */
bool negotiate_websocket_subprotocol(const HttpRequest::ptr &request,
                                     const WebSocketOptions &options,
                                     std::string *selected_subprotocol,
                                     std::string *error);

/**
 * @brief 构造 101 Switching Protocols 握手响应报文
 * @param selected_subprotocol 协商成功的子协议；为空时不输出该响应头。
 */
bool build_websocket_handshake_response(
    const HttpRequest::ptr &request, std::string *response, std::string *error,
    const std::string &selected_subprotocol = "");

/**
 * @brief WebSocket 会话状态机
 * @details
 * 每个升级连接对应一个会话实例：负责触发 on_open、持续解析帧并分发 on_message，
 * 以及在关闭路径上统一触发 on_close/on_error。
 */
class WebSocketSession {
  public:
    using ptr = std::shared_ptr<WebSocketSession>;

    WebSocketSession(std::shared_ptr<znet::TcpConnection> connection,
                     HttpRequest::ptr request, WebSocketCallbacks callbacks,
                     WebSocketOptions options,
                     std::string selected_subprotocol);

    bool on_open();
    bool on_message(znet::Buffer *buffer);
    void on_close();

  private:
    void notify_error(const std::string &error);
    void notify_close(uint16_t close_code, const std::string &reason);

  private:
    WebSocketConnection::ptr connection_;
    HttpRequest::ptr request_;
    WebSocketCallbacks callbacks_;
    WebSocketFrameParser parser_;

    bool close_notified_ = false;
    bool close_frame_sent_ = false;
};

} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_H_
