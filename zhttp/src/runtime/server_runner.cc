#include "runtime/server_runtime.h"

#include "zhttp/http_server.h"
#include "zhttp/runtime/daemon.h"
#include "zhttp/server_config.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <fmt/format.h>

namespace zhttp {
namespace detail {
void run_server(const ServerConfig &config, const ServerFactory &factory) {
    std::string failure;
    auto serve = [&factory, &failure](int /*argc*/, char ** /*argv*/) -> int {
        try {
            // 在 daemon 回调中创建服务，保证守护模式下资源在 fork 后初始化。
            auto server = factory();
            auto started = server->start();
            if (!started)
                throw std::runtime_error(started.error().message());

            // 信号处理只设置停止标志，实际关闭连接由正常执行路径完成。
            while (!Daemon::should_stop() && server->is_running()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            const bool stopped = Daemon::should_stop() || server->stop_requested();
            server->stop();
            if (!stopped)
                failure = "HTTP listener stopped unexpectedly";
            return stopped ? 0 : -1;
        } catch (const std::exception &error) {
            failure = error.what();
            fmt::print(stderr, "Server run failed: {}\n", failure);
            return -1;
        } catch (...) {
            failure = "Unknown server exception";
            fmt::print(stderr, "Server run failed: {}\n", failure);
            return -1;
        }
    };

    int rc =
        Daemon::start_daemon(0, nullptr, std::move(serve), config.daemon);
    if (rc != 0) {
        throw std::runtime_error(failure + "; server exited with code " +
                                 std::to_string(rc));
    }
}

} // namespace detail
} // namespace zhttp
