#include "zhttp/internal/server_runtime.h"

#include "zhttp/daemon.h"
#include "zhttp/http_server.h"
#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zhttp {
namespace detail {
void run_servers(const ServerConfig &config, const ServerFactory &factory) {
    auto run_server = [&config, &factory](int /*argc*/,
                                          char ** /*argv*/) -> int {
        try {
            const ServerPair servers = factory();
            auto server = servers.first;
            auto redirect_server = servers.second;
            ZHTTP_LOG_INFO("Server starting on {}:{}", config.host,
                           config.port);

            if (redirect_server) {
                ZHTTP_LOG_INFO(
                    "Redirect server starting on {}:{} (http -> https)",
                    config.host, config.redirect_http_port);
                if (!redirect_server->start()) {
                    ZHTTP_LOG_ERROR("Redirect server failed to start on {}:{}",
                                    config.host, config.redirect_http_port);
                    return -1;
                }
            }

            if (!server->start()) {
                ZHTTP_LOG_ERROR("Server failed to start on {}:{}", config.host,
                                config.port);
                if (redirect_server) {
                    redirect_server->stop();
                }
                return -1;
            }

            while (!Daemon::should_stop()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            ZHTTP_LOG_INFO("Server stopping on {}:{}", config.host,
                           config.port);
            server->stop();
            if (redirect_server) {
                ZHTTP_LOG_INFO("Redirect server stopping on {}:{}", config.host,
                               config.redirect_http_port);
                redirect_server->stop();
            }
            return 0;
        } catch (const std::exception &ex) {
            ZHTTP_LOG_ERROR("Server run failed: {}", ex.what());
            return -1;
        } catch (...) {
            ZHTTP_LOG_ERROR("Server run failed: unknown exception");
            return -1;
        }
    };

    int rc =
        Daemon::start_daemon(0, nullptr, std::move(run_server), config.daemon);
    if (rc != 0) {
        throw std::runtime_error("Server exited with code " +
                                 std::to_string(rc));
    }
}

} // 命名空间 detail
} // 命名空间 zhttp
