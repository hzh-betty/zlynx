#include "zhttp/http_server.h"
#include "protocol/connection_session.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace zhttp {
namespace {
uint32_t clamp_timeout(uint64_t value) {
    return static_cast<uint32_t>(std::min(
        value, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - 1)));
}
} // namespace
struct HttpServer::Impl {
    Impl(znet::Endpoint endpoint, std::shared_ptr<HttpApplication> application, int backlog)
        : application(application ? std::move(application) : std::make_shared<HttpApplication>()),
          endpoint(std::move(endpoint)) {
        network.backlog = backlog;
        network.write_timeout = std::chrono::milliseconds{30000};
    }
    void check_mutable() const {
        if (frozen)
            throw std::logic_error("HTTP listener configuration is frozen");
    }
    std::shared_ptr<HttpApplication> application;
    detail::HttpProtocolOptions protocol;
    znet::ServerOptions network;
    bool frozen = false;
    std::atomic<bool> stop_requested{false};
    znet::Endpoint endpoint;
    zco::RuntimeOptions runtime_options;
    std::unique_ptr<zco::Runtime> owned_runtime;
    zco::Runtime *runtime = nullptr; // borrowed or owned_runtime.get()
    std::unique_ptr<znet::TcpServer> tcp_server; // destroyed before owned_runtime
};
HttpServer::HttpServer(znet::Endpoint address, zco::RuntimeOptions options, int backlog)
    : impl_(std::make_unique<Impl>(std::move(address), nullptr, backlog)) {
    impl_->runtime_options = options;
}
HttpServer::HttpServer(znet::Endpoint address, zco::Runtime &runtime,
                       std::shared_ptr<HttpApplication> application, int backlog)
    : impl_(std::make_unique<Impl>(std::move(address), std::move(application), backlog)) {
    impl_->runtime = &runtime;
}
HttpServer::~HttpServer() { stop(); }
HttpApplication &HttpServer::application() { return *impl_->application; }
Router &HttpServer::router() { return application().router(); }
void HttpServer::use(mid::Middleware::ptr mw) { application().use(std::move(mw)); }
void HttpServer::use(const std::string &path, mid::Middleware::ptr mw) {
    application().use(path, std::move(mw));
}
void HttpServer::use_group(const std::string &prefix, mid::Middleware::ptr mw) {
    application().use_group(prefix, std::move(mw));
}
void HttpServer::set_not_found_handler(HttpHandler handler) {
    application().set_not_found_handler(std::move(handler));
}
void HttpServer::set_exception_handler(ExceptionHandler handler) {
    application().set_exception_handler(std::move(handler));
}
void HttpServer::set_name(const std::string &name) {
    impl_->check_mutable();
    impl_->protocol.server_name = name;
}
const std::string &HttpServer::name() const { return impl_->protocol.server_name; }
void HttpServer::set_recv_timeout(uint64_t value) {
    impl_->check_mutable();
    impl_->network.read_timeout = std::chrono::milliseconds{clamp_timeout(value)};
}
void HttpServer::set_write_timeout(uint64_t value) {
    impl_->check_mutable();
    impl_->network.write_timeout = std::chrono::milliseconds{clamp_timeout(value)};
}
void HttpServer::set_keepalive_timeout(uint64_t value) {
    impl_->check_mutable();
    impl_->network.idle_timeout = std::chrono::milliseconds{clamp_timeout(value)};
}
void HttpServer::set_request_limits(const RequestLimits &limits) {
    impl_->check_mutable();
    impl_->protocol.limits = limits;
}
void HttpServer::set_request_timeout(uint32_t timeout) {
    impl_->check_mutable();
    impl_->protocol.request_timeout = timeout;
}
void HttpServer::set_error_handler(ErrorHandler handler) {
    impl_->check_mutable();
    impl_->network.on_error = std::move(handler);
}
znet::Result<void> HttpServer::set_ssl_certificate(const std::string &cert, const std::string &key) {
    impl_->check_mutable();
    auto credentials = znet::TlsCredentials::load(cert, key);
    if (!credentials)
        return credentials.error();
    impl_->network.tls = std::make_shared<const znet::TlsCredentials>(std::move(credentials).value());
    return {};
}
znet::Result<void> HttpServer::start() {
    impl_->frozen = true;
    application().freeze();
    if (!impl_->tcp_server) {
        if (!impl_->runtime) {
            impl_->owned_runtime = std::make_unique<zco::Runtime>(impl_->runtime_options);
            impl_->runtime = impl_->owned_runtime.get();
        }
        impl_->tcp_server = std::make_unique<znet::TcpServer>(
            *impl_->runtime, impl_->endpoint,
            detail::http_sessions(impl_->application, impl_->protocol), impl_->network);
    }
    if (!impl_->tcp_server->is_running())
        impl_->stop_requested.store(false, std::memory_order_release);
    return impl_->tcp_server->start();
}
void HttpServer::request_stop() {
    impl_->stop_requested.store(true, std::memory_order_release);
    if (impl_->tcp_server)
        impl_->tcp_server->request_stop();
}
void HttpServer::stop() {
    impl_->stop_requested.store(true, std::memory_order_release);
    if (impl_->tcp_server)
        impl_->tcp_server->stop();
}
bool HttpServer::is_running() const {
    return impl_->tcp_server && impl_->tcp_server->is_running();
}
bool HttpServer::stop_requested() const {
    return impl_->stop_requested.load(std::memory_order_acquire);
}
znet::Result<znet::Endpoint> HttpServer::local_endpoint() const {
    if (!impl_->tcp_server)
        return znet::make_error(znet::ErrorKind::validation, std::errc::not_connected,
                                "HTTP local endpoint", "Listener has not started");
    return impl_->tcp_server->local_endpoint();
}
bool HttpServer::handle(HttpContext &context) { return application().handle(context); }
} // namespace zhttp
