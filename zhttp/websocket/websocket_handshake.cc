#include "zhttp/websocket/websocket_handshake.h"
#include "zhttp/http_response.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
namespace zhttp {
namespace {

constexpr char kWebSocketAcceptGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool is_http_token_char(const unsigned char ch) {
    if (ch <= 0x1F || ch >= 0x7F) {
        return false;
    }

    switch (ch) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t':
        return false;
    default:
        return true;
    }
}

bool is_valid_subprotocol_token(const std::string &token) {
    if (token.empty()) {
        return false;
    }

    for (const unsigned char ch : token) {
        if (!is_http_token_char(ch)) {
            return false;
        }
    }

    return true;
}

bool parse_subprotocol_header(const std::string &header_value,
                              std::vector<std::string> *protocols,
                              std::string *error) {
    if (!protocols || !error) {
        return false;
    }

    protocols->clear();
    error->clear();

    if (header_value.empty()) {
        return true;
    }

    // Sec-WebSocket-Protocol 按 RFC 定义为 token 列表，不允许任意字符。
    const auto tokens = split_string(header_value, ',');
    for (std::string token : tokens) {
        trim(token);
        if (!is_valid_subprotocol_token(token)) {
            *error = "Invalid Sec-WebSocket-Protocol token";
            return false;
        }
        protocols->push_back(std::move(token));
    }

    return true;
}

} // namespace
WebSocketHandshakeResult check_websocket_handshake_request(
    const std::shared_ptr<const HttpRequest> &request, std::string *error) {
    if (!request) {
        if (error) {
            *error = "Request is null";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (request->method() != HttpMethod::GET) {
        if (error) {
            *error = "WebSocket upgrade only supports GET";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (request->version() != HttpVersion::HTTP_1_1) {
        if (error) {
            *error = "WebSocket upgrade requires HTTP/1.1";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    const std::string connection_header = request->header("Connection");
    if (!header_contains_token(connection_header, "upgrade")) {
        if (error) {
            *error = "Missing Connection: Upgrade";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    std::string upgrade_header = request->header("Upgrade");
    trim(upgrade_header);
    if (to_lower(upgrade_header) != "websocket") {
        if (error) {
            *error = "Missing Upgrade: websocket";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    std::string version = request->header("Sec-WebSocket-Version");
    trim(version);
    if (version != "13") {
        if (error) {
            *error = "Sec-WebSocket-Version must be 13";
        }
        return WebSocketHandshakeResult::kUnsupportedVersion;
    }

    std::string key = request->header("Sec-WebSocket-Key");
    trim(key);
    if (key.empty()) {
        if (error) {
            *error = "Missing Sec-WebSocket-Key";
        }
        return WebSocketHandshakeResult::kBadRequest;
    }

    if (error) {
        error->clear();
    }
    return WebSocketHandshakeResult::kOk;
}

std::string compute_websocket_accept_key(const std::string &client_key) {
    const std::string challenge = client_key + kWebSocketAcceptGuid;

    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(challenge.data()),
         challenge.size(), digest);

    unsigned char encoded[((SHA_DIGEST_LENGTH + 2) / 3) * 4 + 1];
    const int encoded_size =
        EVP_EncodeBlock(encoded, digest, SHA_DIGEST_LENGTH);
    if (encoded_size <= 0) {
        return "";
    }

    return std::string(reinterpret_cast<char *>(encoded),
                       static_cast<size_t>(encoded_size));
}

bool negotiate_websocket_subprotocol(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, std::string *selected_subprotocol,
    std::string *error) {
    if (!selected_subprotocol || !error) {
        return false;
    }

    selected_subprotocol->clear();
    error->clear();

    if (!request) {
        *error = "Request is null";
        return false;
    }

    std::vector<std::string> client_protocols;
    if (!parse_subprotocol_header(request->header("Sec-WebSocket-Protocol"),
                                  &client_protocols, error)) {
        return false;
    }

    if (options.subprotocols.empty() || client_protocols.empty()) {
        // 任一方未声明子协议时，握手可继续，但不返回 Sec-WebSocket-Protocol。
        return true;
    }

    // 服务器按自身优先级选择第一个命中的子协议，保持行为可预测。
    for (const std::string &server_protocol : options.subprotocols) {
        if (!is_valid_subprotocol_token(server_protocol)) {
            *error = "Invalid server WebSocket subprotocol configuration";
            return false;
        }

        for (const std::string &client_protocol : client_protocols) {
            if (client_protocol == server_protocol) {
                *selected_subprotocol = server_protocol;
                return true;
            }
        }
    }

    *error = "No compatible Sec-WebSocket-Protocol";
    return false;
}

bool prepare_websocket_handshake(
    const std::shared_ptr<const HttpRequest> &request,
    const WebSocketOptions &options, HttpResponse &response,
    std::string &selected, std::string &error) {
    const auto result = check_websocket_handshake_request(request, &error);
    if (result != WebSocketHandshakeResult::kOk ||
        !negotiate_websocket_subprotocol(request, options, &selected, &error)) {
        response.status(HttpStatus::BAD_REQUEST)
            .text("Bad WebSocket Request: " + error);
        response.set_keep_alive(false);
        if (result == WebSocketHandshakeResult::kUnsupportedVersion)
            response.header("Sec-WebSocket-Version", "13");
        return false;
    }
    response.status(HttpStatus::SWITCHING_PROTOCOLS).body("");
    response.set_version(HttpVersion::HTTP_1_1);
    response.header("Upgrade", "websocket")
        .header(
            "Sec-WebSocket-Accept",
            compute_websocket_accept_key(request->header("Sec-WebSocket-Key")));
    if (!selected.empty())
        response.header("Sec-WebSocket-Protocol", selected);
    return true;
}
} // namespace zhttp
