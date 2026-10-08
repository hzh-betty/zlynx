#ifndef ZHTTP_INTERNAL_SERVER_RUNTIME_H_
#define ZHTTP_INTERNAL_SERVER_RUNTIME_H_

#include <functional>
#include <memory>

namespace zhttp {
class HttpServer;
struct ServerConfig;
namespace detail {

using ServerFactory = std::function<std::shared_ptr<HttpServer>()>;

// 服务初始化保留现有的全局日志和协程栈配置策略。
void configure_server_runtime(const ServerConfig &config);
std::shared_ptr<HttpServer> create_http_server(const ServerConfig &config);

// 服务工厂在守护进程回调中执行，保持 fork 后创建服务的原有时机。
void run_server(const ServerConfig &config, const ServerFactory &factory);

} // namespace detail
} // namespace zhttp

#endif
