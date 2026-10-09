#include "zhttp/http_server.h"
#include "zhttp/pipeline/request_pipeline.h"
#include "zhttp/protocol/http_protocol_handler.h"
#include "znet/server/tcp_server.h"
#include "zhttp/zhttp_logger.h"
#include <limits>
namespace zhttp {
namespace {
// 会话闭包持有每连接协议状态；协议切换在当前回调返回之后完成。
struct ConnectionState {
    std::unique_ptr<ProtocolHandler> current, pending;
    std::weak_ptr<znet::Connection> connection;
    void drive(znet::ByteBuffer &buffer) {
        auto conn = connection.lock();
        if (!conn)
            return;
        current->on_data(conn, buffer);
        // 先等 HTTP 回调返回，再交接协议，避免升级过程中销毁正在执行的 this。
        // 握手之后可能已有 WebSocket 帧留在同一缓冲区，切换后立即继续消费。
        if (pending && conn->connected()) {
            current = std::move(pending);
            if (current->on_open() && buffer.readable_bytes())
                current->on_data(conn, buffer);
        }
    }
    void close() {
        if (current)
            current->on_closed();
        pending.reset();
        current.reset();
    }
};
uint32_t clamp_timeout(uint64_t value) {
    return static_cast<uint32_t>(std::min(
        value,
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - 1)));
}
} // namespace
struct HttpServer::Runtime {
    Router router;
    RequestPipeline pipeline;
    RequestLimits limits;
    uint32_t timeout = 30000;
    std::string name = "zhttp/1.0";
    bool frozen = false;
    znet::ServerOptions network;
};

HttpServer::HttpServer(znet::Endpoint address, zco::RuntimeOptions options,
                       int backlog)
    : runtime_(std::make_shared<Runtime>()),
      io_runtime_(std::make_unique<zco::Runtime>(options)),
      endpoint_(std::move(address)) {
    runtime_->network.backlog = backlog;
    runtime_->network.write_timeout = std::chrono::milliseconds{30000};
    runtime_->network.on_error = [](const znet::Error &error) {
        ZHTTP_LOG_WARN("Network session failed: {}", error.message());
    };
}
HttpServer::~HttpServer() { stop(); }
void HttpServer::check_mutable() const {
    if (runtime_->frozen)
        throw std::logic_error("HTTP configuration is frozen");
}
Router &HttpServer::router() { return runtime_->router; }
void HttpServer::use(mid::Middleware::ptr mw) {
    runtime_->pipeline.use(std::move(mw));
}
void HttpServer::use(const std::string &path, mid::Middleware::ptr mw) {
    runtime_->pipeline.use(path, std::move(mw));
}
void HttpServer::use_group(const std::string &prefix, mid::Middleware::ptr mw) {
    runtime_->pipeline.use_group(prefix, std::move(mw));
}
void HttpServer::set_not_found_handler(HttpHandler handler) {
    runtime_->pipeline.set_not_found_handler(std::move(handler));
}
void HttpServer::set_exception_handler(ExceptionHandler handler) {
    runtime_->pipeline.set_exception_handler(std::move(handler));
}

void HttpServer::set_name(const std::string &name) {
    check_mutable();
    runtime_->name = name;
}

const std::string &HttpServer::name() const { return runtime_->name; }

void HttpServer::set_recv_timeout(uint64_t value) {
    check_mutable();
    runtime_->network.read_timeout =
        std::chrono::milliseconds{clamp_timeout(value)};
}

void HttpServer::set_write_timeout(uint64_t value) {
    check_mutable();
    runtime_->network.write_timeout =
        std::chrono::milliseconds{clamp_timeout(value)};
}

void HttpServer::set_keepalive_timeout(uint64_t value) {
    check_mutable();
    runtime_->network.idle_timeout =
        std::chrono::milliseconds{clamp_timeout(value)};
}
void HttpServer::set_request_limits(const RequestLimits &limits) {
    check_mutable();
    runtime_->limits = limits;
}
void HttpServer::set_request_timeout(uint32_t timeout) {
    check_mutable();
    runtime_->timeout = timeout;
}
bool HttpServer::set_ssl_certificate(const std::string &cert,
                                     const std::string &key) {
    check_mutable();
    auto credentials = znet::TlsCredentials::load(cert, key);
    if (!credentials)
        return false;
    runtime_->network.tls = std::make_shared<const znet::TlsCredentials>(
        std::move(credentials).value());
    return true;
}

bool HttpServer::start() {
    // 启动前冻结共享配置，使各连接仅并发读取路由和中间件注册表。
    runtime_->frozen = true;
    runtime_->router.freeze();
    runtime_->pipeline.freeze();
    if (!tcp_server_) {
        auto runtime = runtime_;
        znet::SessionFactory factory =
            [runtime](const znet::Connection::ptr &conn) {
                auto state = std::make_shared<ConnectionState>();
                state->connection = conn;
                std::weak_ptr<ConnectionState> weak = state;
                state->current = std::make_unique<HttpProtocolHandler>(
                    runtime->router, runtime->pipeline, runtime->limits,
                    runtime->timeout, runtime->name,
                    [weak](std::unique_ptr<ProtocolHandler> pending) {
                        if (auto state = weak.lock())
                            state->pending = std::move(pending);
                    });
                return znet::SessionCallbacks{
                    [state](const znet::Connection::ptr &,
                            znet::ByteBuffer &buffer) { state->drive(buffer); },
                    [state](const znet::Connection::ptr &) { state->close(); }};
            };
        tcp_server_ = std::make_unique<znet::TcpServer>(
            *io_runtime_, endpoint_, std::move(factory), runtime_->network);
    }
    auto started = tcp_server_->start();
    if (!started)
        ZHTTP_LOG_ERROR("Network server failed to start: {}", started.error().message());
    return static_cast<bool>(started);
}
void HttpServer::stop() {
    if (tcp_server_)
        tcp_server_->stop();
}
bool HttpServer::is_running() const {
    return tcp_server_ && tcp_server_->is_running();
}
bool HttpServer::handle(HttpContext &context) {
    return runtime_->pipeline.execute(context, runtime_->router);
}
} // namespace zhttp
