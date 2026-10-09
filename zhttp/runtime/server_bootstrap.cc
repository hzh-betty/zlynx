#include "zhttp/runtime/server_runtime.h"

#include "zco/coroutine.h"
#include "zhttp/http_server.h"
#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"
#include "znet/endpoint.h"
#include <algorithm>
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

} // namespace

void configure_server_runtime(const ServerConfig &config) {
    configure_unified_logging(config);
    ZHTTP_LOG_INFO("Creating server with {} threads, stack_mode={}",
                   config.num_threads, stack_mode_to_string(config.stack_mode));
}

std::shared_ptr<HttpServer> create_http_server(const ServerConfig &config) {
    auto addrs = znet::resolve_endpoints(config.host, config.port);
    if (!addrs) {
        throw std::runtime_error("Failed to resolve address: " + config.host +
                                 ":" + std::to_string(config.port));
    }

    // 创建服务器。HTTPS 与 HTTP 统一在 HttpServer 内部处理，避免双分支实现。
    zco::RuntimeOptions options;
    options.worker_count = config.num_threads;
    options.stack_model = config.stack_mode;
    auto server = std::make_shared<HttpServer>(addrs.value().front(), options);
    if (config.enable_https &&
        !server->set_ssl_certificate(config.cert_file, config.key_file)) {
        throw std::runtime_error("Failed to initialize SSL certificate");
    }

    server->set_name(config.server_name);
    server->set_recv_timeout(config.read_timeout);
    server->set_write_timeout(config.write_timeout);
    server->set_keepalive_timeout(config.keepalive_timeout);

    return server;
}

} // namespace detail
} // namespace zhttp
