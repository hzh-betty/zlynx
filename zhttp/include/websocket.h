#ifndef ZHTTP_WEBSOCKET_H_
#define ZHTTP_WEBSOCKET_H_

#include "http_request.h"
#include "websocket_frame.h"
#include "znet/tcp_connection.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace zhttp {

struct WebSocketOptions {
  size_t max_message_size = kDefaultWebSocketMaxMessageSize;
};

class WebSocketConnection {
public:
  using ptr = std::shared_ptr<WebSocketConnection>;

  explicit WebSocketConnection(std::weak_ptr<znet::TcpConnection> connection);

  bool send_text(const std::string &message);
  bool send_binary(const std::string &payload);
  bool ping(const std::string &payload = "");
  bool pong(const std::string &payload = "");
  bool close(WebSocketCloseCode code = WebSocketCloseCode::kNormalClosure,
             const std::string &reason = "");

  bool connected() const;
  int fd() const;

  void mark_closed();

private:
  bool send_frame(WebSocketOpcode opcode,
                  const std::string &payload,
                  bool fin = true);

private:
  std::weak_ptr<znet::TcpConnection> connection_;
  std::atomic<bool> closed_;
};

using WebSocketOpenCallback =
    std::function<void(const WebSocketConnection::ptr &, const HttpRequest::ptr &)>;

using WebSocketMessageCallback = std::function<void(const WebSocketConnection::ptr &,
                                                    std::string &&,
                                                    WebSocketMessageType)>;

using WebSocketCloseCallback = std::function<void(const WebSocketConnection::ptr &,
                                                  uint16_t,
                                                  const std::string &)>;

using WebSocketErrorCallback =
    std::function<void(const WebSocketConnection::ptr &, const std::string &)>;

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

WebSocketHandshakeResult check_websocket_handshake_request(
    const HttpRequest::ptr &request,
    std::string *error);

std::string compute_websocket_accept_key(const std::string &client_key);

bool build_websocket_handshake_response(const HttpRequest::ptr &request,
                                        std::string *response,
                                        std::string *error);

class WebSocketSession {
public:
  using ptr = std::shared_ptr<WebSocketSession>;

  WebSocketSession(std::shared_ptr<znet::TcpConnection> connection,
                   HttpRequest::ptr request,
                   WebSocketCallbacks callbacks,
                   WebSocketOptions options);

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
