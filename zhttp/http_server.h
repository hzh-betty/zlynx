#ifndef ZHTTP_HTTP_SERVER_H_
#define ZHTTP_HTTP_SERVER_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/request_limits.h"
#include "zhttp/router/router.h"
#include "znet/address.h"
#include <cstdint>
#include <exception>
#include <memory>
#include <sys/socket.h>
namespace znet {
class TcpServer;
}
namespace zhttp {
/** 异常处理回调；接收当前上下文与捕获的异常。 */
using ExceptionHandler = std::function<void(HttpContext &, std::exception_ptr)>;
/**
 * 基于协程 TCP 服务的 HTTP/HTTPS 和 WebSocket 服务器。
 *
 * 在 start() 前完成路由、中间件、TLS 与运行参数配置。
 * start() 冻结配置，即使监听启动失败也不能继续修改；已注册回调可能并发执行。
 */
class HttpServer {
  public:
    using ptr = std::shared_ptr<HttpServer>;
    /**
     * 构造服务器，不启动监听。
     *
     * @param address 监听地址。
     * @param backlog 待接受连接队列长度，默认 SOMAXCONN。
     */
    explicit HttpServer(znet::Address::ptr address, int backlog = SOMAXCONN);
    /** 停止服务器并释放运行资源。 */
    virtual ~HttpServer();
    /** 返回路由器，须在 start() 前完成注册。 */
    Router &router();
    /** 追加全局中间件，按注册顺序 before、逆序 after。 */
    void use(mid::Middleware::ptr middleware);
    /** 追加指定请求路径的中间件，按字面路径匹配。 */
    void use(const std::string &path, mid::Middleware::ptr middleware);
    /** 追加路径组中间件；匹配 prefix 下的子路径，不匹配 prefix 本身。 */
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    /** 设置未匹配路由时的处理器。 */
    void set_not_found_handler(HttpHandler handler);
    /** 设置异常处理器；空回调恢复默认 500 处理。 */
    void set_exception_handler(ExceptionHandler handler);
    /** 设置 Server 响应头的值。 */
    void set_name(const std::string &name);
    /** 返回服务器名称引用。 */
    const std::string &name() const;
    /** 设置 IO 线程数量，超过 int 最大值时截断。 */
    void set_thread_count(size_t count);
    /** 设置网络读取超时，单位毫秒，0 表示关闭。 */
    void set_recv_timeout(uint64_t timeout);
    /** 设置网络写出超时，单位毫秒，0 表示关闭。 */
    void set_write_timeout(uint64_t timeout);
    /** 设置保持连接空闲超时，单位毫秒，0 表示关闭。 */
    void set_keepalive_timeout(uint64_t timeout);
    /** 设置请求解析资源上限。 */
    void set_request_limits(const RequestLimits &limits);
    /** 设置从请求首字节起的读取期限，单位毫秒，0 表示关闭。 */
    void set_request_timeout(uint32_t timeout);
    /**
     * 为当前监听器启用 TLS。
     *
     * @param cert PEM 证书文件路径。
     * @param key PEM 私钥文件路径。
     * @return 证书及 TLS 上下文初始化成功返回 true，否则 false。
     */
    bool set_ssl_certificate(const std::string &cert, const std::string &key);
    /**
     * 冻结配置并启动监听，调用后返回。
     *
     * @return 网络服务启动成功返回 true，否则 false。
     */
    bool start();
    /** 停止监听和现有连接。 */
    void stop();
    /** 返回服务器当前是否运行。 */
    bool is_running() const;
    /**
     * 在当前线程执行路由及中间件，供离线请求处理使用。
     *
     * @param context 已绑定请求的上下文，响应由处理链构造。
     * @return 路由是否命中；不表示响应是否写出成功。
     */
    bool handle(HttpContext &context);

  protected:
    std::shared_ptr<znet::TcpServer> tcp_server() const { return tcp_server_; }

  private:
    void check_mutable() const;
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<znet::TcpServer> tcp_server_;
};
} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_H_
