#ifndef ZHTTP_HTTP_SERVER_H_
#define ZHTTP_HTTP_SERVER_H_

#include "zco/runtime.h"
#include "zhttp/http_application.h"
#include "zhttp/request_limits.h"
#include "znet/endpoint.h"
#include <cstdint>
#include <memory>
#include <sys/socket.h>

namespace zhttp {
/** HTTP/HTTPS 与 WebSocket 监听器。
 * start/stop/析构在控制线程调用；请求回调使用 request_stop()。
 * 自有 Runtime 在首次 start() 创建；借用 Runtime 必须比服务器活得更久。
 * start() 冻结应用与监听配置，包括启动失败的情况。
 */
class HttpServer {
  public:
    using ptr = std::shared_ptr<HttpServer>;
    using ErrorHandler = std::function<void(const znet::Error &)>;
    explicit HttpServer(znet::Endpoint address, zco::RuntimeOptions options,
                        int backlog = SOMAXCONN);
    HttpServer(znet::Endpoint address, zco::Runtime &runtime,
               std::shared_ptr<HttpApplication> application = {}, int backlog = SOMAXCONN);
    virtual ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;
    HttpApplication &application();
    Router &router();
    void use(mid::Middleware::ptr middleware);
    void use(const std::string &path, mid::Middleware::ptr middleware);
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    void set_not_found_handler(HttpHandler handler);
    void set_exception_handler(ExceptionHandler handler);
    void set_name(const std::string &name);
    const std::string &name() const;
    void set_recv_timeout(uint64_t timeout);
    void set_write_timeout(uint64_t timeout);
    void set_keepalive_timeout(uint64_t timeout);
    void set_request_limits(const RequestLimits &limits);
    void set_request_timeout(uint32_t timeout);
    /** 会话终态错误接收位置；回调可能并发调用，不能抛出异常。 */
    void set_error_handler(ErrorHandler handler);
    znet::Result<void> set_ssl_certificate(const std::string &cert, const std::string &key);
    znet::Result<void> start();
    /** 关闭监听与连接，不等待当前请求，可在请求回调中使用。 */
    void request_stop();
    /** 发起停止并等待所有会话结束；只能从控制线程调用。 */
    void stop();
    bool is_running() const;
    bool stop_requested() const;
    znet::Result<znet::Endpoint> local_endpoint() const;
    /** 离线执行便利入口，等同 application().handle(context)。 */
    bool handle(HttpContext &context);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zhttp
#endif
