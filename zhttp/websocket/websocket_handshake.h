#pragma once
#include "zhttp/websocket/websocket_handler.h"
namespace zhttp {
enum class WebSocketHandshakeResult {
    kOk,
    kBadRequest,
    kUnsupportedVersion,
};

WebSocketHandshakeResult check_websocket_handshake_request(
    const std::shared_ptr<const HttpRequest> &request, std::string *error);

std::string compute_websocket_accept_key(const std::string &client_key);

bool negotiate_websocket_subprotocol(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, std::string *selected_subprotocol,
    std::string *error);

class HttpResponse;
bool prepare_websocket_handshake(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, HttpResponse &response,
    std::string &selected, std::string &error);
} // namespace zhttp
