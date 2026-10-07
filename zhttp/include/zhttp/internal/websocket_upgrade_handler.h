#ifndef ZHTTP_INTERNAL_WEBSOCKET_UPGRADE_HANDLER_H_
#define ZHTTP_INTERNAL_WEBSOCKET_UPGRADE_HANDLER_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace znet {
class TcpConnection;
}
namespace zhttp {
class HttpRequest;
class HttpResponse;
class WebSocketSession;
namespace detail {

// 负责从 HTTP 升级到 WebSocket，并管理升级后建立的会话。
class WebSocketUpgradeHandler {
  public:
    bool upgrade(const std::shared_ptr<znet::TcpConnection> &conn,
                 const std::shared_ptr<HttpRequest> &request,
                 const HttpResponse &response, const std::string &server_name);
    bool is_active(const std::shared_ptr<znet::TcpConnection> &conn) const;
    std::shared_ptr<WebSocketSession> find(int fd) const;
    void register_session(int fd, std::shared_ptr<WebSocketSession> session);
    std::shared_ptr<WebSocketSession> take(int fd);

  private:
    mutable std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<WebSocketSession>> sessions_;
};
} // 命名空间 detail
} // 命名空间 zhttp

#endif
