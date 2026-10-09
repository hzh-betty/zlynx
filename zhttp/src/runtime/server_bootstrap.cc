#include "runtime/server_runtime.h"

#include "zhttp/http_server.h"
#include "zhttp/server_config.h"
#include "runtime/logging.h"
#include "znet/endpoint.h"
#include <stdexcept>

namespace zhttp {
namespace detail {
std::shared_ptr<HttpServer> create_http_server(const ServerConfig &config) {
    auto addrs = znet::resolve_endpoints(config.host, config.port);
    if (!addrs) {
        throw std::runtime_error(addrs.error().message());
    }

    // 创建服务器。HTTPS 与 HTTP 统一在 HttpServer 内部处理，避免双分支实现。
    zco::RuntimeOptions options;
    options.worker_count = config.num_threads;
    options.stack_model = config.stack_mode;
    auto server = std::make_shared<HttpServer>(addrs.value().front(), options);
    if (config.enable_https) {
        auto tls = server->set_ssl_certificate(config.cert_file, config.key_file);
        if (!tls)
            throw std::runtime_error(tls.error().message());
    }
    server->set_error_handler(network_error_logger(config.log_level));

    server->set_name(config.server_name);
    server->set_recv_timeout(config.read_timeout);
    server->set_write_timeout(config.write_timeout);
    server->set_keepalive_timeout(config.keepalive_timeout);

    return server;
}

} // namespace detail
} // namespace zhttp
