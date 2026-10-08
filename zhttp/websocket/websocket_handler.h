#pragma once
#include "zhttp/http_request.h"
#include "zhttp/websocket/websocket_connection.h"
#include "zhttp/websocket/websocket_types.h"
#include <functional>
namespace zhttp {
using WebSocketOpenCallback =
    std::function<void(const WebSocketConnection::ptr &,
                       const std::shared_ptr<const HttpRequest> &)>;

using WebSocketMessageCallback = std::function<void(
    const WebSocketConnection::ptr &, std::string &&, WebSocketMessageType)>;

using WebSocketCloseCallback = std::function<void(
    const WebSocketConnection::ptr &, uint16_t, const std::string &)>;

using WebSocketErrorCallback =
    std::function<void(const WebSocketConnection::ptr &, const std::string &)>;

struct WebSocketCallbacks {
    WebSocketOpenCallback on_open;
    WebSocketMessageCallback on_message;
    WebSocketCloseCallback on_close;
    WebSocketErrorCallback on_error;
};

struct WebSocketUpgrade {
    WebSocketCallbacks callbacks;
    WebSocketOptions options;
};

} // namespace zhttp
