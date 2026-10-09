#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_HANDLER_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_HANDLER_H_

#include "zhttp/http_request.h"
#include "zhttp/websocket/websocket_connection.h"
#include "zhttp/websocket/websocket_types.h"
#include <functional>
namespace zhttp {
/** 握手写出并切换协议后调用，携带只读握手请求。 */
using WebSocketOpenCallback =
    std::function<void(const WebSocketConnection::ptr &,
                       const std::shared_ptr<const HttpRequest> &)>;

/** 完整消息回调，接管消息字节并提供文本/二进制类型。 */
using WebSocketMessageCallback = std::function<void(
    const WebSocketConnection::ptr &, std::string &&, WebSocketMessageType)>;

/** 关闭通知，携带关闭码与原因，最多通知一次。 */
using WebSocketCloseCallback = std::function<void(
    const WebSocketConnection::ptr &, uint16_t, const std::string &)>;

/** 协议错误通知，携带错误说明。 */
using WebSocketErrorCallback =
    std::function<void(const WebSocketConnection::ptr &, const std::string &)>;

/** 连接生命周期回调集合；均在连接处理流程中调用。 */
struct WebSocketCallbacks {
    WebSocketOpenCallback on_open;
    WebSocketMessageCallback on_message;
    WebSocketCloseCallback on_close;
    WebSocketErrorCallback on_error;
};

/** 请求的协议升级意图；由 HttpContext 交给网络协议层。 */
struct WebSocketUpgrade {
    WebSocketCallbacks callbacks;
    WebSocketOptions options;
};

} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_WEBSOCKET_HANDLER_H_
