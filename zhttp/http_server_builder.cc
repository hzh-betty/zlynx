/**
 * http_server_builder.cc
 * http_server_builder 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/http_server_builder.h"
#include "zhttp/middleware/request_body_middleware.h"
#include "zhttp/runtime/server_runtime.h"

#include <stdexcept>
#include <utility>

namespace zhttp {

HttpServerBuilder::HttpServerBuilder() {
    // 使用默认配置
}

HttpServerBuilder &
HttpServerBuilder::from_config(const std::string &config_path) {
    config_ = ServerConfig::from_toml(config_path);
    return *this;
}

HttpServerBuilder &HttpServerBuilder::read_timeout(uint64_t timeout_ms) {
    config_.read_timeout = timeout_ms;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::write_timeout(uint64_t timeout_ms) {
    config_.write_timeout = timeout_ms;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::keepalive_timeout(uint64_t timeout_ms) {
    config_.keepalive_timeout = timeout_ms;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::listen(const std::string &host,
                                             uint16_t port) {
    config_.host = host;
    config_.port = port;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::threads(size_t num_threads) {
    config_.num_threads = num_threads;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::stack_mode(zco::StackModel mode) {
    config_.stack_mode = mode;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::use_shared_stack() {
    config_.stack_mode = zco::StackModel::kShared;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::use_independent_stack() {
    config_.stack_mode = zco::StackModel::kIndependent;
    return *this;
}

HttpServerBuilder &
HttpServerBuilder::enable_https(const std::string &cert_file,
                                const std::string &key_file) {
    config_.enable_https = true;
    config_.cert_file = cert_file;
    config_.key_file = key_file;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::use(mid::Middleware::ptr middleware) {
    if (middleware) {
        middlewares_.push_back(std::move(middleware));
    }
    return *this;
}

HttpServerBuilder &HttpServerBuilder::get(const std::string &path,
                                          RouterCallback callback) {
    routes_.emplace_back(HttpMethod::GET, path, std::move(callback));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::get(const std::string &path,
                                          RouteHandler::ptr handler) {
    routes_.emplace_back(HttpMethod::GET, path,
                         make_route_callback(std::move(handler)));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::post(const std::string &path,
                                           RouterCallback callback) {
    routes_.emplace_back(HttpMethod::POST, path, std::move(callback));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::post(const std::string &path,
                                           RouteHandler::ptr handler) {
    routes_.emplace_back(HttpMethod::POST, path,
                         make_route_callback(std::move(handler)));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::put(const std::string &path,
                                          RouterCallback callback) {
    routes_.emplace_back(HttpMethod::PUT, path, std::move(callback));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::put(const std::string &path,
                                          RouteHandler::ptr handler) {
    routes_.emplace_back(HttpMethod::PUT, path,
                         make_route_callback(std::move(handler)));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::del(const std::string &path,
                                          RouterCallback callback) {
    routes_.emplace_back(HttpMethod::DELETE, path, std::move(callback));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::del(const std::string &path,
                                          RouteHandler::ptr handler) {
    routes_.emplace_back(HttpMethod::DELETE, path,
                         make_route_callback(std::move(handler)));
    return *this;
}

HttpServerBuilder &
HttpServerBuilder::websocket(const std::string &path,
                             WebSocketCallbacks callbacks,
                             const WebSocketOptions &options) {
    auto callbacks_ref =
        std::make_shared<WebSocketCallbacks>(std::move(callbacks));

    routes_.emplace_back(
        HttpMethod::GET, path,
        RouterCallback([callbacks_ref, options](HttpContext &context) {
            context.upgrade_to_websocket(*callbacks_ref, options);
        }));
    return *this;
}

HttpServerBuilder &HttpServerBuilder::not_found(RouterCallback callback) {
    not_found_handler_ = std::move(callback);
    return *this;
}

HttpServerBuilder &HttpServerBuilder::not_found(RouteHandler::ptr handler) {
    not_found_handler_ = make_route_callback(std::move(handler));
    return *this;
}

HttpServerBuilder &
HttpServerBuilder::exception_handler(ExceptionHandler handler) {
    exception_handler_ = std::move(handler);
    return *this;
}

HttpServerBuilder &HttpServerBuilder::log_level(const std::string &level) {
    config_.log_level = level;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::daemon(bool enable) {
    config_.daemon = enable;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::homepage(const std::string &path) {
    config_.homepage = path;
    return *this;
}

HttpServerBuilder &HttpServerBuilder::server_name(const std::string &name) {
    config_.server_name = name;
    return *this;
}

std::shared_ptr<HttpServer> HttpServerBuilder::build() {
    // 验证配置
    if (!config_.validate()) {
        throw std::runtime_error("Invalid server configuration");
    }

    detail::configure_server_runtime(config_);
    auto server = detail::create_http_server(config_);

    if (!config_.homepage.empty()) {
        server->router().set_homepage(config_.homepage);
    }

    // 默认启用请求体解析：JSON / x-www-form-urlencoded / multipart。
    server->use(std::make_shared<mid::RequestBodyMiddleware>());

    // 注册中间件
    for (auto &mw : middlewares_) {
        server->use(mw);
    }

    // 注册路由
    for (auto &route : routes_) {
        HttpMethod method = std::get<0>(route);
        const std::string &path = std::get<1>(route);
        RouterCallback &handler = std::get<2>(route);

        server->router().add_route(method, path, handler);
    }

    // 设置 404 处理器
    if (not_found_handler_) {
        server->set_not_found_handler(not_found_handler_);
    }

    // 设置异常处理器
    if (exception_handler_) {
        server->set_exception_handler(exception_handler_);
    }

    return server;
}

void HttpServerBuilder::run() {
    detail::run_server(config_, [this]() { return build(); });
}

} // namespace zhttp
