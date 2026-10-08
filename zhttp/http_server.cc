#include "zhttp/http_server.h"
#include "zhttp/pipeline/request_pipeline.h"
#include "zhttp/protocol/http_protocol_handler.h"
#include "znet/tcp_server.h"
#include <limits>
namespace zhttp {
namespace {
// 每个 TCP 连接独占协议状态；configuration 延长共享配置的寿命。
struct ConnectionState {
    std::unique_ptr<ProtocolHandler> current, pending;
    std::weak_ptr<znet::TcpConnection> connection;
    std::shared_ptr<void> configuration;
    bool driving = false, closed = false;
    void drive(znet::Buffer &buffer) {
        // 发送或关闭回调可能重入，保护正在执行的协议对象不被递归驱动。
        if (driving || closed)
            return;
        auto conn = connection.lock();
        if (!conn)
            return;
        driving = true;
        try {
            current->on_data(conn, buffer);
            // 先等 HTTP 回调返回，再交接协议，避免升级过程中销毁正在执行的 this。
            // 握手之后可能已有 WebSocket 帧留在同一缓冲区，切换后立即继续消费。
            if (pending && conn->connected()) {
                current = std::move(pending);
                if (current->on_open() && buffer.readable_bytes())
                    current->on_data(conn, buffer);
            }
        } catch (...) {
            conn->close();
        }
        driving = false;
    }
    void close() {
        if (closed)
            return;
        closed = true;
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
};

HttpServer::HttpServer(znet::Address::ptr address, zco::RuntimeOptions options,
                       int backlog)
    : runtime_(std::make_shared<Runtime>()),
      io_runtime_(new zco::Runtime(options)),
      tcp_server_(std::make_shared<znet::TcpServer>(
          *io_runtime_, std::move(address), backlog)) {
    auto runtime = runtime_;
    tcp_server_->set_on_connection(
        [runtime](const znet::TcpConnection::ptr &conn) {
            auto state = std::make_shared<ConnectionState>();
            state->configuration = runtime;
            state->connection = conn;
            // 切换回调只借用连接状态，避免 state 与 current 形成共享所有权环。
            std::weak_ptr<ConnectionState> weak = state;
            state->current.reset(new HttpProtocolHandler(
                runtime->router, runtime->pipeline, runtime->limits,
                runtime->timeout, runtime->name,
                [weak](std::unique_ptr<ProtocolHandler> pending) {
                    if (auto state = weak.lock())
                        state->pending = std::move(pending);
                }));
            conn->set_context(
                new std::shared_ptr<ConnectionState>(std::move(state)));
        });
    tcp_server_->set_on_message([](const znet::TcpConnection::ptr &conn,
                                   znet::Buffer &buffer) {
        auto *state =
            static_cast<std::shared_ptr<ConnectionState> *>(conn->context());
        if (state)
            (*state)->drive(buffer);
    });
    tcp_server_->set_on_close([](const znet::TcpConnection::ptr &conn) {
        auto *state =
            static_cast<std::shared_ptr<ConnectionState> *>(conn->context());
        if (state) {
            (*state)->close();
            delete state;
            conn->set_context(nullptr);
        }
    });
    tcp_server_->set_write_timeout(30000);
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
    tcp_server_->set_read_timeout(clamp_timeout(value));
}
void HttpServer::set_write_timeout(uint64_t value) {
    check_mutable();
    tcp_server_->set_write_timeout(clamp_timeout(value));
}
void HttpServer::set_keepalive_timeout(uint64_t value) {
    check_mutable();
    tcp_server_->set_keepalive_timeout(value);
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
    return tcp_server_->enable_tls(cert, key);
}
bool HttpServer::start() {
    // 启动前冻结共享配置，使各连接仅并发读取路由和中间件注册表。
    runtime_->frozen = true;
    runtime_->router.freeze();
    runtime_->pipeline.freeze();
    return tcp_server_->start();
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
