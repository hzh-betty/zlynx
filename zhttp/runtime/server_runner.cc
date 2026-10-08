#include "zhttp/runtime/server_runtime.h"

#include "zhttp/http_server.h"
#include "zhttp/runtime/daemon.h"
#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zhttp {
namespace detail {
void run_server(const ServerConfig &config, const ServerFactory &factory) {
    auto serve = [&config, &factory](int /*argc*/, char ** /*argv*/) -> int {
        try {
            // 在 daemon 回调中创建服务，保证守护模式下资源在 fork 后初始化。
            auto server = factory();
            ZHTTP_LOG_INFO("Server starting on {}:{}", config.host,
                           config.port);

            if (!server->start()) {
                ZHTTP_LOG_ERROR("Server failed to start on {}:{}", config.host,
                                config.port);
                return -1;
            }

            // 信号处理只设置停止标志，实际关闭连接由正常执行路径完成。
            while (!Daemon::should_stop()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            ZHTTP_LOG_INFO("Server stopping on {}:{}", config.host,
                           config.port);
            server->stop();
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
        Daemon::start_daemon(0, nullptr, std::move(serve), config.daemon);
    if (rc != 0) {
        throw std::runtime_error("Server exited with code " +
                                 std::to_string(rc));
    }
}

} // namespace detail
} // namespace zhttp
