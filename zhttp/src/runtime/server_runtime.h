#ifndef ZHTTP_INTERNAL_SERVER_RUNTIME_H_
#define ZHTTP_INTERNAL_SERVER_RUNTIME_H_

#include <functional>
#include <memory>

namespace zhttp {
class HttpServer;
struct ServerConfig;
namespace detail {

/** 服务构造回调，返回单个 HTTP 或 HTTPS 监听器。 */
using ServerFactory = std::function<std::shared_ptr<HttpServer>()>;

/** 创建并配置单个监听器；地址或 TLS 初始化失败抛出 std::runtime_error。 */
std::shared_ptr<HttpServer> create_http_server(const ServerConfig &config);

// 服务工厂在守护进程回调中执行，保持 fork 后创建服务的原有时机。
/** 通过 daemon 回调构造并运行服务至停止信号，失败抛出 std::runtime_error。 */
void run_server(const ServerConfig &config, const ServerFactory &factory);

} // namespace detail
} // namespace zhttp

#endif
