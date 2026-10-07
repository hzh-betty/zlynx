#include "zhttp/internal/websocket_upgrade_handler.h"

#include "zhttp/http_response.h"
#include "zhttp/internal/http_response_writer.h"
#include "zhttp/websocket.h"
#include "zhttp/zhttp_logger.h"
#include <utility>

namespace zhttp {
namespace detail {
namespace {
HttpResponse make_websocket_handshake_error_response(
    const HttpVersion version, const std::string &server_name,
    const WebSocketHandshakeResult result, const std::string &error) {
    HttpResponse response;
    response.set_version(version);
    response.set_keep_alive(false);
    response.header("Server", server_name);

    if (result == WebSocketHandshakeResult::kUnsupportedVersion) {
        response.status(HttpStatus::BAD_REQUEST)
            .header("Sec-WebSocket-Version", "13")
            .text("Bad WebSocket Request: " + error);
    } else {
        response.status(HttpStatus::BAD_REQUEST)
            .text("Bad WebSocket Request: " + error);
    }

    return response;
}
} // 命名空间

bool WebSocketUpgradeHandler::is_active(
    const znet::TcpConnection::ptr &conn) const {
    if (!conn) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    return sessions_.find(conn->fd()) != sessions_.end();
}

WebSocketSession::ptr WebSocketUpgradeHandler::find(const int fd) const {
    if (fd < 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

void WebSocketUpgradeHandler::register_session(const int fd,
                                               WebSocketSession::ptr session) {
    if (fd < 0 || !session) {
        return;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    sessions_[fd] = std::move(session);
}

WebSocketSession::ptr WebSocketUpgradeHandler::take(const int fd) {
    if (fd < 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        return nullptr;
    }

    auto session = std::move(it->second);
    sessions_.erase(it);
    return session;
}

bool WebSocketUpgradeHandler::upgrade(
    const std::shared_ptr<znet::TcpConnection> &conn,
    const std::shared_ptr<HttpRequest> &request, const HttpResponse &response,
    const std::string &server_name) {
    std::string error;
    const WebSocketHandshakeResult check_result =
        check_websocket_handshake_request(request, &error);
    if (check_result != WebSocketHandshakeResult::kOk) {
        const HttpResponse reject_response =
            make_websocket_handshake_error_response(
                request->version(), server_name, check_result, error);
        const std::string payload = reject_response.serialize();
        if (!send_all_or_fail(conn, payload.data(), payload.size())) {
            ZHTTP_LOG_WARN("Send WebSocket handshake rejection failed: fd={}",
                           conn->fd());
        }
        return false;
    }

    std::string selected_subprotocol;
    if (!negotiate_websocket_subprotocol(request, response.websocket_options(),
                                         &selected_subprotocol, &error)) {
        HttpResponse reject_response;
        reject_response.set_version(request->version());
        reject_response.set_keep_alive(false);
        reject_response.header("Server", server_name);
        reject_response.status(HttpStatus::BAD_REQUEST)
            .text("Bad WebSocket Request: " + error);

        const std::string payload = reject_response.serialize();
        if (!send_all_or_fail(conn, payload.data(), payload.size())) {
            ZHTTP_LOG_WARN("Send WebSocket subprotocol negotiation failure "
                           "response failed: fd={}",
                           conn->fd());
        }
        return false;
    }

    std::string handshake_response;
    if (!build_websocket_handshake_response(request, &handshake_response,
                                            &error, selected_subprotocol)) {
        HttpResponse reject_response;
        reject_response.set_version(request->version());
        reject_response.set_keep_alive(false);
        reject_response.header("Server", server_name);
        reject_response.status(HttpStatus::BAD_REQUEST)
            .text("Bad WebSocket Request: " + error);

        const std::string payload = reject_response.serialize();
        if (!send_all_or_fail(conn, payload.data(), payload.size())) {
            ZHTTP_LOG_WARN(
                "Send WebSocket handshake failure response failed: fd={}",
                conn->fd());
        }
        return false;
    }

    if (!send_all_or_fail(conn, handshake_response.data(),
                          handshake_response.size())) {
        ZHTTP_LOG_WARN("Send WebSocket handshake response failed: fd={}",
                       conn->fd());
        return false;
    }

    auto session = std::make_shared<WebSocketSession>(
        conn, request, response.websocket_callbacks(),
        response.websocket_options(), selected_subprotocol);
    register_session(conn->fd(), session);

    if (!session->on_open()) {
        take(conn->fd());
        return false;
    }

    ZHTTP_LOG_DEBUG("WebSocket upgrade success: fd={}, path={}, subprotocol={}",
                    conn->fd(), request->path(),
                    selected_subprotocol.empty() ? "<none>"
                                                 : selected_subprotocol);
    return true;
}

} // 命名空间 detail
} // 命名空间 zhttp
