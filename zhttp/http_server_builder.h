/**
 * http_server_builder.h
 * http_server_builder 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_HTTP_SERVER_BUILDER_H_
#define ZHTTP_HTTP_SERVER_BUILDER_H_

#include "zhttp/http_server.h"
#include "zhttp/middleware/middleware.h"
#include "zhttp/router/route_handler.h"
#include "zhttp/server_config.h"
#include "zhttp/websocket/websocket_handler.h"

#include <memory>
#include <string>

namespace zhttp {

/**
 * HTTP 服务器建造者
 *
 * Builder 负责把“配置项、中间件、路由、日志等级”等离散信息收集起来，
 * 最终一次性构造出可运行的 HttpServer。这样可以避免调用方手工拼装对象时
 * 到处分散配置，也更适合从配置文件启动服务。
 */
class HttpServerBuilder {
  public:
    /** 使用默认 ServerConfig 构造 Builder。 */
    HttpServerBuilder();

    /**
     * 从 TOML 配置文件加载
     *
     * @param config_path TOML 配置文件路径
     * @return 当前 Builder 引用，便于继续链式配置
     */
    HttpServerBuilder &from_config(const std::string &config_path);

    /**
     * 设置读取超时
     *
     * @param timeout_ms 读取超时时间，单位毫秒；0 表示关闭
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &read_timeout(uint64_t timeout_ms);

    /**
     * 设置写出超时
     *
     * @param timeout_ms 写出超时时间，单位毫秒；0 表示关闭
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &write_timeout(uint64_t timeout_ms);

    /**
     * 设置 Keep-Alive 空闲超时
     *
     * @param timeout_ms 空闲超时时间，单位毫秒；0 表示关闭
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &keepalive_timeout(uint64_t timeout_ms);

    /**
     * 设置监听地址
     *
     * @param host 监听地址，例如 0.0.0.0
     * @param port 监听端口
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &listen(const std::string &host, uint16_t port);

    /**
     * 设置线程数
     *
     * @param num_threads IO/工作线程数量
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &threads(size_t num_threads);

    /**
     * 设置协程栈模式
     *
     * @param mode 栈模式 (INDEPENDENT 或 SHARED)
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &stack_mode(zco::StackModel mode);

    /**
     * 使用共享栈模式
     *
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &use_shared_stack();

    /**
     * 使用独立栈模式（默认）
     *
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &use_independent_stack();

    /**
     * 启用HTTPS
     *
     * @param cert_file 证书文件路径
     * @param key_file 私钥文件路径
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &enable_https(const std::string &cert_file,
                                    const std::string &key_file);

    /**
     * 添加全局中间件
     *
     * @param middleware 中间件对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &use(mid::Middleware::ptr middleware);

    /**
     * 注册GET路由（回调方式）
     *
     * @param path 路由路径
     * @param callback 处理回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &get(const std::string &path, RouterCallback callback);

    /**
     * 注册GET路由（处理器方式）
     *
     * @param path 路由路径
     * @param handler 处理器对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &get(const std::string &path, RouteHandler::ptr handler);

    /**
     * 注册POST路由（回调方式）
     *
     * @param path 路由路径
     * @param callback 处理回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &post(const std::string &path, RouterCallback callback);

    /**
     * 注册POST路由（处理器方式）
     *
     * @param path 路由路径
     * @param handler 处理器对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &post(const std::string &path, RouteHandler::ptr handler);

    /**
     * 注册PUT路由（回调方式）
     *
     * @param path 路由路径
     * @param callback 处理回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &put(const std::string &path, RouterCallback callback);

    /**
     * 注册PUT路由（处理器方式）
     *
     * @param path 路由路径
     * @param handler 处理器对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &put(const std::string &path, RouteHandler::ptr handler);

    /**
     * 注册DELETE路由（回调方式）
     *
     * @param path 路由路径
     * @param callback 处理回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &del(const std::string &path, RouterCallback callback);

    /**
     * 注册DELETE路由（处理器方式）
     *
     * @param path 路由路径
     * @param handler 处理器对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &del(const std::string &path, RouteHandler::ptr handler);

    /**
     * 注册 WebSocket 路由
     *
     * @param path 路由路径
     * @param callbacks WebSocket 生命周期回调
     * @param options WebSocket 协议参数
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &
    websocket(const std::string &path, WebSocketCallbacks callbacks,
              const WebSocketOptions &options = WebSocketOptions());

    /**
     * 设置404处理器（回调方式）
     *
     * @param callback 404 回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &not_found(RouterCallback callback);

    /**
     * 设置404处理器（处理器方式）
     *
     * @param handler 404 处理器对象
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &not_found(RouteHandler::ptr handler);

    /**
     * 设置异常处理回调
     *
     * @param handler 异常处理回调
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &exception_handler(ExceptionHandler handler);

    /**
     * 设置日志级别
     *
     * @param level 日志级别字符串，例如 info、debug
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &log_level(const std::string &level);

    /**
     * 启用守护进程模式
     *
     * @param enable 是否启用
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &daemon(bool enable = true);

    /**
     * 设置首页跳转目标
     *
     * @param path 首页目标路径或绝对 URL
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &homepage(const std::string &path);

    /**
     * 设置服务器名称
     *
     * @param name Server 响应头里显示的名称
     * @return 当前 Builder 引用
     */
    HttpServerBuilder &server_name(const std::string &name);

    /**
     * 构建服务器，完成初始化但不启动监听
     *
     * 调用 start() 启动监听，或通过 run() 构建并阻塞运行。
     *
     * @return 构建完成的服务器对象
     * @throws std::runtime_error 配置无效、地址解析或 TLS 初始化失败。
     */
    std::shared_ptr<HttpServer> build();

    /**
     * 构建并运行服务器（阻塞）
     *
     * 适合 main 函数里直接调用的场景。
     */
    void run();

    /**
     * 获取当前配置
     *
     * @return 当前配置的只读引用，其生命周期与 Builder 相同
     */
    const ServerConfig &config() const { return config_; }

  private:
    // 当前构建中的服务器配置。
    ServerConfig config_;

    // 待注册到服务器上的全局中间件和路由。
    std::vector<mid::Middleware::ptr> middlewares_;
    std::vector<std::tuple<HttpMethod, std::string, RouterCallback>> routes_;

    // 可选的自定义 404 处理器。
    RouterCallback not_found_handler_;

    // 可选的自定义异常处理器。
    ExceptionHandler exception_handler_;

};

} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_BUILDER_H_
