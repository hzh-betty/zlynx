#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_HANDSHAKE_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_HANDSHAKE_H_

#include "zhttp/websocket/websocket_handler.h"


/*
    客户端发送 HTTP 升级请求
        ↓
    HttpProtocolHandler 解析请求，调用业务路由
        ↓
    业务调用 context.upgrade_to_websocket(...)
        ↓
    prepare_websocket_handshake(...)
        ├─ check_websocket_handshake_request(...)
        ├─ negotiate_websocket_subprotocol(...)
        └─ compute_websocket_accept_key(...)
        ↓
    成功：准备 101 响应，创建候选 WebSocketProtocolHandler
    失败：准备 400 响应，要求关闭连接
        ↓
    HttpResponseWriter 写出响应
        ↓
    成功写出且允许继续连接，才登记协议切换
        ↓
    ConnectionSession 接管新处理器，调用 on_open()
*/

namespace zhttp {
/**
 * @brief 握手校验结果，区分一般错误和不支持的协议版本。
 */
enum class WebSocketHandshakeResult {
    kOk,
    kBadRequest,
    kUnsupportedVersion,
};

/**
 * @brief 校验 GET、HTTP/1.1 和升级必需字段；error 输出失败原因。
 */
WebSocketHandshakeResult check_websocket_handshake_request(
    const std::shared_ptr<const HttpRequest> &request, std::string *error);

/**
 * @brief 计算 Sec-WebSocket-Accept：客户端 key 与协议 GUID 拼接后 SHA-1 并
 * Base64。
 */
std::string compute_websocket_accept_key(const std::string &client_key);

/**
 * @brief 按服务器偏好选择共同子协议；任一方未声明时成功并输出空子协议。
 */
bool negotiate_websocket_subprotocol(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, std::string *selected_subprotocol,
    std::string *error);

class HttpResponse;

/**
 * @brief 校验握手并构造 101 响应；失败构造 400、关闭连接并输出错误。
 */
bool prepare_websocket_handshake(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, HttpResponse &response,
    std::string &selected, std::string &error);
} // namespace zhttp

#endif
