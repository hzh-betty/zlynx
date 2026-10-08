#include "zhttp/runtime/server_runtime.h"

#include "zco/sched.h"
#include "zhttp/http_server.h"
#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"
#include "znet/address.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace zhttp {
namespace detail {
namespace {
static zlog::LogLevel::value parse_log_level(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (s == "debug") {
        return zlog::LogLevel::value::DEBUG;
    }
    if (s == "info") {
        return zlog::LogLevel::value::INFO;
    }
    if (s == "warning" || s == "warn") {
        return zlog::LogLevel::value::WARNING;
    }
    if (s == "error") {
        return zlog::LogLevel::value::ERROR;
    }
    if (s == "fatal") {
        return zlog::LogLevel::value::FATAL;
    }
    return zlog::LogLevel::value::INFO;
}

static void configure_unified_logging(const ServerConfig &config) {
    zhttp::init_logger(parse_log_level(config.log_level));
}

static bool is_any_address_host(const std::string &host) {
    return host == "0.0.0.0" || host == "::" || host == "[::]";
}

static std::string strip_host_port(const std::string &host_header) {
    if (host_header.empty()) {
        return "";
    }

    // IPv6 地址去除端口：[::1]:8080 -> [::1]。
    if (host_header.front() == '[') {
        const std::size_t end = host_header.find(']');
        if (end != std::string::npos) {
            return host_header.substr(0, end + 1);
        }
        return host_header;
    }

    const std::size_t first_colon = host_header.find(':');
    if (first_colon == std::string::npos) {
        return host_header;
    }

    // 没有 [] 包裹但出现多个冒号，通常是 IPv6 字面量，保持原值。
    if (host_header.find(':', first_colon + 1) != std::string::npos) {
        return host_header;
    }

    return host_header.substr(0, first_colon);
}

static std::string make_https_location(const HttpContext &request,
                                       const ServerConfig &config) {
    std::string host = strip_host_port(request.header("Host"));
    if (host.empty()) {
        host = config.host;
        if (is_any_address_host(host)) {
            host = "localhost";
        }
    }

    std::string target = "https://" + host;
    if (config.port != 443) {
        target += ":" + std::to_string(config.port);
    }

    const std::string &path = request.path();
    target += path.empty() ? "/" : path;

    const std::string &query = request.query();
    if (!query.empty()) {
        target += "?";
        target += query;
    }

    return target;
}

static void install_force_https_redirect_routes(HttpServer &redirect_server,
                                                const ServerConfig &config) {
    static const std::array<HttpMethod, 9> kMethods = {
        HttpMethod::GET,    HttpMethod::POST,    HttpMethod::PUT,
        HttpMethod::DELETE, HttpMethod::HEAD,    HttpMethod::OPTIONS,
        HttpMethod::PATCH,  HttpMethod::CONNECT, HttpMethod::TRACE};

    auto redirect_handler = [config](HttpContext &context) {
        auto *req = &context;
        auto &resp = context.response();
        resp.redirect(make_https_location(*req, config),
                      HttpStatus::PERMANENT_REDIRECT);
    };

    // 同时兜底根路径和任意子路径。
    for (HttpMethod method : kMethods) {
        redirect_server.router().add_route(method, "/", redirect_handler);
        redirect_server.router().add_route(method, "/*path", redirect_handler);
    }
}

} // namespace

void configure_server_runtime(const ServerConfig &config) {
    configure_unified_logging(config);
    zco::co_stack_model(config.stack_mode);
    ZHTTP_LOG_INFO("Creating server with {} threads, stack_mode={}",
                   config.num_threads, stack_mode_to_string(config.stack_mode));
}

std::shared_ptr<HttpServer> create_http_server(const ServerConfig &config) {
    auto addrs = znet::Address::lookup(config.host, config.port);
    if (addrs.empty()) {
        throw std::runtime_error("Failed to resolve address: " + config.host +
                                 ":" + std::to_string(config.port));
    }

    // 创建服务器。HTTPS 与 HTTP 统一在 HttpServer 内部处理，避免双分支实现。
    auto server = std::make_shared<HttpServer>(addrs[0]);
    if (config.enable_https &&
        !server->set_ssl_certificate(config.cert_file, config.key_file)) {
        throw std::runtime_error("Failed to initialize SSL certificate");
    }

    server->set_thread_count(config.num_threads);
    server->set_name(config.server_name);
    server->set_recv_timeout(config.read_timeout);
    server->set_write_timeout(config.write_timeout);
    server->set_keepalive_timeout(config.keepalive_timeout);

    return server;
}

std::shared_ptr<HttpServer>
create_https_redirect_server(const ServerConfig &config) {
    if (!config.enable_https || !config.force_http_to_https)
        return nullptr;
    auto redirect_addrs =
        znet::Address::lookup(config.host, config.redirect_http_port);
    if (redirect_addrs.empty()) {
        throw std::runtime_error(
            "Failed to resolve redirect address: " + config.host + ":" +
            std::to_string(config.redirect_http_port));
    }

    auto redirect_server = std::make_shared<HttpServer>(redirect_addrs[0]);
    redirect_server->set_thread_count(1);
    redirect_server->set_name(config.server_name + " (redirect)");
    redirect_server->set_recv_timeout(config.read_timeout);
    redirect_server->set_write_timeout(config.write_timeout);
    redirect_server->set_keepalive_timeout(config.keepalive_timeout);

    install_force_https_redirect_routes(*redirect_server, config);
    return redirect_server;
}

} // namespace detail
} // namespace zhttp
