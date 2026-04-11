#include "http_server.h"

#include "zhttp_logger.h"

#include <atomic>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zhttp {

namespace {

struct HttpConnectionContext {
    HttpParser parser;
    std::string remote_addr;
};

uint32_t clamp_timeout_to_u32(const uint64_t timeout_ms) {
    const uint64_t max_timeout =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1;
    if (timeout_ms >= max_timeout) {
        return static_cast<uint32_t>(max_timeout);
    }
    return static_cast<uint32_t>(timeout_ms);
}

bool is_body_allowed(const HttpStatus status) {
    const int code = static_cast<int>(status);
    if (code >= 100 && code < 200) {
        return false;
    }
    return code != 204 && code != 304;
}

bool send_all_or_fail(const znet::TcpConnection::ptr &conn, const char *data,
                      size_t length) {
    if (length == 0) {
        return true;
    }

    if (!conn) {
        return false;
    }

    return conn->send(data, length) >= 0;
}

bool send_chunk_frame(const znet::TcpConnection::ptr &conn, const char *data,
                      size_t length) {
    // RFC 7230 分块格式：<hex-size> CRLF <chunk-data> CRLF。
    char size_line[32];
    const int size_len =
        std::snprintf(size_line, sizeof(size_line), "%zx\r\n", length);
    if (size_len <= 0 || static_cast<size_t>(size_len) >= sizeof(size_line)) {
        return false;
    }

    if (!send_all_or_fail(conn, size_line, static_cast<size_t>(size_len))) {
        return false;
    }
    if (!send_all_or_fail(conn, data, length)) {
        return false;
    }
    return send_all_or_fail(conn, "\r\n", 2);
}

bool send_chunked_body(const znet::TcpConnection::ptr &conn,
                       const HttpResponse &response) {
    if (response.has_stream_callback()) {
        // 同步流式：服务器循环“拉取”业务层生成的数据，每次产出一个 chunk。
        constexpr size_t kStreamChunkBufferSize = 8192;
        std::vector<char> stream_buffer(kStreamChunkBufferSize);
        const auto &callback = response.stream_callback();

        while (true) {
            const size_t produced =
                callback(stream_buffer.data(), stream_buffer.size());
            if (produced == 0) {
                break;
            }
            if (produced > stream_buffer.size()) {
                return false;
            }
            if (!send_chunk_frame(conn, stream_buffer.data(), produced)) {
                return false;
            }
        }
    } else if (!response.body_content().empty()) {
        // 非流式 chunked：把完整 body 当作一个 chunk 发出。
        if (!send_chunk_frame(conn, response.body_content().data(),
                              response.body_content().size())) {
            return false;
        }
    }

    // 发送终止块（size=0）和空 trailer，标记 chunked body 结束。
    return send_all_or_fail(conn, "0\r\n\r\n", 5);
}

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

} // namespace

HttpServer::HttpServer(znet::Address::ptr listen_address, int backlog)
    : tcp_server_(std::make_shared<znet::TcpServer>(std::move(listen_address),
                                                    backlog)) {
    tcp_server_->set_on_connection(
        [this](const znet::TcpConnection::ptr &conn) { on_connection(conn); });
    tcp_server_->set_on_message(
        [this](const znet::TcpConnection::ptr &conn, znet::Buffer &buffer) {
            on_message(conn, buffer);
        });
    tcp_server_->set_on_close(
        [this](const znet::TcpConnection::ptr &conn) { on_close(conn); });

    // 默认把 Server 响应头和底层服务名都设置成统一值。
    set_name("zhttp/1.0");
}

HttpServer::~HttpServer() {
    if (tcp_server_) {
        tcp_server_->stop();
    }
}

void HttpServer::set_name(const std::string &name) { server_name_ = name; }

void HttpServer::set_thread_count(size_t thread_count) {
    if (!tcp_server_) {
        return;
    }

    const size_t max_count =
        static_cast<size_t>(std::numeric_limits<int>::max());
    if (thread_count > max_count) {
        thread_count = max_count;
    }
    tcp_server_->set_thread_count(static_cast<int>(thread_count));
}

void HttpServer::set_recv_timeout(uint64_t timeout_ms) {
    if (!tcp_server_) {
        return;
    }
    tcp_server_->set_read_timeout(clamp_timeout_to_u32(timeout_ms));
}

void HttpServer::set_write_timeout(uint64_t timeout_ms) {
    if (!tcp_server_) {
        return;
    }
    tcp_server_->set_write_timeout(clamp_timeout_to_u32(timeout_ms));
}

void HttpServer::set_keepalive_timeout(uint64_t timeout_ms) {
    if (!tcp_server_) {
        return;
    }
    tcp_server_->set_keepalive_timeout(timeout_ms);
}

bool HttpServer::set_ssl_certificate(const std::string &cert_file,
                                     const std::string &key_file) {
    if (!tcp_server_) {
        return false;
    }

    return tcp_server_->enable_tls(cert_file, key_file);
}

bool HttpServer::start() {
    if (!tcp_server_) {
        return false;
    }
    return tcp_server_->start();
}

void HttpServer::stop() {
    if (tcp_server_) {
        tcp_server_->stop();
    }
}

bool HttpServer::is_running() const {
    return tcp_server_ && tcp_server_->is_running();
}

bool HttpServer::is_async_stream_active(
    const znet::TcpConnection::ptr &conn) const {
    if (!conn) {
        return false;
    }

    std::lock_guard<std::mutex> guard(async_stream_mutex_);
    return async_stream_fds_.find(conn->fd()) != async_stream_fds_.end();
}

void HttpServer::mark_async_stream_active(int fd) {
    if (fd < 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(async_stream_mutex_);
    async_stream_fds_.insert(fd);
}

void HttpServer::mark_async_stream_finished(int fd) {
    if (fd < 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(async_stream_mutex_);
    async_stream_fds_.erase(fd);
}

bool HttpServer::send_async_chunked_response(
    const znet::TcpConnection::ptr &conn, const HttpResponse &response) {
    if (!conn || !response.has_async_stream_callback()) {
        return false;
    }

    // 异步推送也必须先发送响应头，后续才允许逐块写 body。
    const std::string headers = response.serialize(false);
    if (!send_all_or_fail(conn, headers.data(), headers.size())) {
        return false;
    }

    const int fd = conn->fd();
    mark_async_stream_active(fd);

    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto write_mutex = std::make_shared<std::mutex>();
    auto finish_stream = std::make_shared<std::function<void(bool)>>();

    *finish_stream = [this, conn, fd, closed, write_mutex](bool send_terminal) {
        // 结束流程只允许执行一次：避免重复 close 或重复发送终止块。
        bool expected = false;
        if (!closed->compare_exchange_strong(expected, true)) {
            return;
        }

        if (send_terminal) {
            // 与 sender 共用同一把写锁，保证 chunk 帧与终止块不会交叉。
            std::lock_guard<std::mutex> guard(*write_mutex);
            if (!send_all_or_fail(conn, "0\r\n\r\n", 5)) {
                ZHTTP_LOG_WARN(
                    "Send HTTP async chunked terminal frame failed: fd={}",
                    conn ? conn->fd() : -1);
            }
        }

        mark_async_stream_finished(fd);
        conn->shutdown();
    };

    HttpResponse::AsyncChunkSender sender =
        [conn, closed, write_mutex, finish_stream](const std::string &chunk) {
            // 空 chunk 不编码为“零长度数据块”，终止必须通过 closer 触发。
            if (chunk.empty() || closed->load(std::memory_order_acquire)) {
                return !closed->load(std::memory_order_acquire);
            }

            bool send_ok = false;
            {
                std::lock_guard<std::mutex> guard(*write_mutex);
                if (closed->load(std::memory_order_acquire)) {
                    return false;
                }
                send_ok = send_chunk_frame(conn, chunk.data(), chunk.size());
            }

            if (!send_ok) {
                (*finish_stream)(false);
                return false;
            }

            return true;
        };

    // closer 负责最终发送终止块并关闭连接。
    HttpResponse::AsyncStreamCloser closer = [finish_stream]() {
        (*finish_stream)(true);
    };

    try {
        response.async_stream_callback()(std::move(sender), std::move(closer));
    } catch (...) {
        (*finish_stream)(false);
        return false;
    }

    return true;
}

bool HttpServer::is_websocket_active(
    const znet::TcpConnection::ptr &conn) const {
    if (!conn) {
        return false;
    }

    std::lock_guard<std::mutex> guard(websocket_mutex_);
    return websocket_sessions_.find(conn->fd()) != websocket_sessions_.end();
}

WebSocketSession::ptr HttpServer::find_websocket_session(const int fd) const {
    if (fd < 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(websocket_mutex_);
    auto it = websocket_sessions_.find(fd);
    if (it == websocket_sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

void HttpServer::register_websocket_session(const int fd,
                                            WebSocketSession::ptr session) {
    if (fd < 0 || !session) {
        return;
    }

    std::lock_guard<std::mutex> guard(websocket_mutex_);
    websocket_sessions_[fd] = std::move(session);
}

WebSocketSession::ptr HttpServer::take_websocket_session(const int fd) {
    if (fd < 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(websocket_mutex_);
    auto it = websocket_sessions_.find(fd);
    if (it == websocket_sessions_.end()) {
        return nullptr;
    }

    auto session = std::move(it->second);
    websocket_sessions_.erase(it);
    return session;
}

HttpParser *HttpServer::ensure_parser(const znet::TcpConnection::ptr &conn) {
    if (!conn) {
        return nullptr;
    }

    auto *ctx = static_cast<HttpConnectionContext *>(conn->context());
    if (!ctx) {
        ctx = new HttpConnectionContext();
        if (conn->socket()) {
            auto remote_addr = conn->socket()->get_remote_address();
            if (remote_addr) {
                ctx->remote_addr = remote_addr->to_string();
            }
        }
        conn->set_context(ctx);
    }
    return &ctx->parser;
}

void HttpServer::on_connection(const znet::TcpConnection::ptr &conn) {
    if (!conn) {
        return;
    }

    ensure_parser(conn);
    ZHTTP_LOG_DEBUG("New connection: fd={}", conn->fd());
}

void HttpServer::on_close(const znet::TcpConnection::ptr &conn) {
    if (!conn) {
        return;
    }

    mark_async_stream_finished(conn->fd());

    auto websocket_session = take_websocket_session(conn->fd());
    if (websocket_session) {
        websocket_session->on_close();
    }

    auto *ctx = static_cast<HttpConnectionContext *>(conn->context());
    delete ctx;
    conn->set_context(nullptr);

    ZHTTP_LOG_DEBUG("Connection closed: fd={}", conn->fd());
}

void HttpServer::on_message(const znet::TcpConnection::ptr &conn,
                            znet::Buffer &buffer) {
    if (!conn) {
        return;
    }

    auto websocket_session = find_websocket_session(conn->fd());
    if (websocket_session) {
        if (!websocket_session->on_message(&buffer)) {
            take_websocket_session(conn->fd());
        }
        return;
    }

    /**
     * 这里的循环有两个目的：
     * 1. 处理一个缓冲区里可能连续到达的多个请求。
     * 2. 在请求不完整时尽早返回，等下次网络数据到来后继续。
     */

    // 解析器挂在连接上下文里，拆包时可以跨多次 on_message 持续推进状态机。
    auto *parser = ensure_parser(conn);
    if (!parser) {
        return;
    }

    while (buffer.readable_bytes() > 0) {
        if (is_async_stream_active(conn) || is_websocket_active(conn)) {
            // 异步流式写出期间暂停该连接的后续请求解析，避免响应交叉。
            return;
        }

        ParseResult result = parser->parse(&buffer);

        if (result == ParseResult::COMPLETE) {
            // 一条完整请求已经拿到，可以交给业务层处理。
            const bool keep_alive = handle_request(conn, parser->request());

            if (is_async_stream_active(conn) || is_websocket_active(conn)) {
                return;
            }

            // 客户端如果不希望保持连接，就在当前响应发完后主动关闭。
            if (!keep_alive) {
                conn->shutdown();
                return;
            }

            // 连接仍然保持时，继续尝试解析缓冲区里后续可能已经到达的请求。
            parser->reset();
        } else if (result == ParseResult::NEED_MORE) {
            // 半包场景，等待下一次 on_message 再继续解析。
            return;
        } else if (result == ParseResult::ERROR) {
            // 请求报文不合法时直接返回 400，并关闭连接，避免后续状态混乱。
            ZHTTP_LOG_WARN("HTTP parse error: {}", parser->error());
            HttpResponse response;
            response.status(HttpStatus::BAD_REQUEST)
                .content_type("text/plain")
                .body("Bad Request: " + parser->error());
            response.set_keep_alive(false);
            const std::string payload = response.serialize();
            if (conn->send(payload.data(), payload.size()) < 0) {
                ZHTTP_LOG_WARN("Send HTTP 400 failed: fd={}", conn->fd());
            }
            conn->shutdown();
            return;
        }
    }
}

bool HttpServer::handle_request(const znet::TcpConnection::ptr &conn,
                                const HttpRequest::ptr &request) {
    ZHTTP_LOG_DEBUG("{} {} {}", method_to_string(request->method()),
                    request->path(), version_to_string(request->version()));

    // 把对端地址补进请求对象，便于日志、鉴权、限流等上层逻辑直接读取。
    if (conn) {
        auto *ctx = static_cast<HttpConnectionContext *>(conn->context());
        if (ctx && !ctx->remote_addr.empty()) {
            request->set_remote_addr(ctx->remote_addr);
        }
    }

    // 响应对象的协议版本和 Keep-Alive 策略通常跟随请求。
    HttpResponse response;
    response.set_version(request->version());
    response.set_keep_alive(request->is_keep_alive());
    response.header("Server", server_name_);

    // 路由器内部会完成匹配、中间件执行和业务处理器调用。
    router_.route(request, response);

    if (response.has_websocket_upgrade()) {
        std::string error;
        const WebSocketHandshakeResult check_result =
            check_websocket_handshake_request(request, &error);
        if (check_result != WebSocketHandshakeResult::kOk) {
            const HttpResponse reject_response =
                make_websocket_handshake_error_response(
                    request->version(), server_name_, check_result, error);
            const std::string payload = reject_response.serialize();
            if (!send_all_or_fail(conn, payload.data(), payload.size())) {
                ZHTTP_LOG_WARN(
                    "Send WebSocket handshake rejection failed: fd={}",
                    conn->fd());
            }
            return false;
        }

        std::string selected_subprotocol;
        if (!negotiate_websocket_subprotocol(request,
                                             response.websocket_options(),
                                             &selected_subprotocol, &error)) {
            HttpResponse reject_response;
            reject_response.set_version(request->version());
            reject_response.set_keep_alive(false);
            reject_response.header("Server", server_name_);
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
            reject_response.header("Server", server_name_);
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
        register_websocket_session(conn->fd(), session);

        if (!session->on_open()) {
            take_websocket_session(conn->fd());
            return false;
        }

        ZHTTP_LOG_DEBUG(
            "WebSocket upgrade success: fd={}, path={}, subprotocol={}",
            conn->fd(), request->path(),
            selected_subprotocol.empty() ? "<none>" : selected_subprotocol);
        return true;
    }

    const bool use_chunked =
        is_body_allowed(response.status_code()) &&
        response.version() == HttpVersion::HTTP_1_1 &&
        (response.is_chunked_enabled() || response.has_stream_callback() ||
         response.has_async_stream_callback());

    if (!use_chunked) {
        // 非 chunked 路径一次性序列化并发送完整报文。
        std::string response_str = response.serialize();
        if (conn->send(response_str.data(), response_str.size()) < 0) {
            ZHTTP_LOG_WARN("Send HTTP response failed: fd={}", conn->fd());
            return false;
        }
    } else if (response.has_async_stream_callback()) {
        // 异步 chunked 路径交给业务层推送，完成后由 close 回调结束连接。
        try {
            if (!send_async_chunked_response(conn, response)) {
                ZHTTP_LOG_WARN("Send HTTP async chunked response failed: fd={}",
                               conn->fd());
                return false;
            }
        } catch (const std::exception &ex) {
            ZHTTP_LOG_WARN(
                "Async chunked response handler threw: fd={}, error={}",
                conn->fd(), ex.what());
            return false;
        } catch (...) {
            ZHTTP_LOG_WARN("Async chunked response handler threw: fd={}",
                           conn->fd());
            return false;
        }

        // 若业务层已同步 close，这里会返回 false，让上层执行收尾；
        // 若仍在异步推送中，则返回 true，等待 close 回调主动关连接。
        return is_async_stream_active(conn);
    } else {
        // chunked 路径先发响应头，再发送分块体与终止块。
        const std::string headers = response.serialize(false);
        if (!send_all_or_fail(conn, headers.data(), headers.size()) ||
            !send_chunked_body(conn, response)) {
            ZHTTP_LOG_WARN("Send HTTP chunked response failed: fd={}",
                           conn->fd());
            return false;
        }
    }

    ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                    status_to_string(response.status_code()));

    return response.is_keep_alive();
}

} // namespace zhttp
