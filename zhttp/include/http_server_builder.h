#ifndef ZHTTP_HTTP_SERVER_BUILDER_H_
#define ZHTTP_HTTP_SERVER_BUILDER_H_

#include "http_server.h"
#include "middleware.h"
#include "route_handler.h"
#include "server_config.h"

#include <memory>
#include <string>

namespace zhttp {

/**
 * @brief HTTP服务器建造者
 * 提供链式调用API和TOML配置两种初始化方式
 */
class HttpServerBuilder {
public:
  HttpServerBuilder();

  // ========== 从配置初始化 ==========

  /**
   * @brief 从 TOML 配置文件加载
   * @param config_path TOML 配置文件路径
   */
  HttpServerBuilder &from_config(const std::string &config_path);

  /**
   * @brief 从 ServerConfig 对象初始化
   */
  HttpServerBuilder &from_config(const ServerConfig &config);

  // ========== 链式配置 API ==========

  /**
   * @brief 设置监听地址
   */
  HttpServerBuilder &listen(const std::string &host, uint16_t port);

  /**
   * @brief 设置线程数
   */
  HttpServerBuilder &threads(size_t num_threads);

  /**
   * @brief 设置协程栈模式
   * @param mode 栈模式 (INDEPENDENT 或 SHARED)
   */
  HttpServerBuilder &stack_mode(StackMode mode);

  /**
   * @brief 使用共享栈模式
   */
  HttpServerBuilder &use_shared_stack();

  /**
   * @brief 使用独立栈模式（默认）
   */
  HttpServerBuilder &use_independent_stack();

  /**
   * @brief 启用HTTPS
   */
  HttpServerBuilder &enable_https(const std::string &cert_file,
                                  const std::string &key_file);

  /**
   * @brief 添加全局中间件
   */
  HttpServerBuilder &use(Middleware::ptr middleware);

  /**
   * @brief 注册GET路由（回调方式）
   */
  HttpServerBuilder &get(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册GET路由（处理器方式）
   */
  HttpServerBuilder &get(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册POST路由（回调方式）
   */
  HttpServerBuilder &post(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册POST路由（处理器方式）
   */
  HttpServerBuilder &post(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册PUT路由（回调方式）
   */
  HttpServerBuilder &put(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册PUT路由（处理器方式）
   */
  HttpServerBuilder &put(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册DELETE路由（回调方式）
   */
  HttpServerBuilder &del(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册DELETE路由（处理器方式）
   */
  HttpServerBuilder &del(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 设置404处理器（回调方式）
   */
  HttpServerBuilder &not_found(RouterCallback callback);

  /**
   * @brief 设置404处理器（处理器方式）
   */
  HttpServerBuilder &not_found(RouteHandler::ptr handler);

  /**
   * @brief 设置日志级别
   */
  HttpServerBuilder &log_level(const std::string &level);

  /**
   * @brief 启用守护进程模式
   */
  HttpServerBuilder &daemon(bool enable = true);

  /**
   * @brief 设置服务器名称
   */
  HttpServerBuilder &server_name(const std::string &name);

  /**
   * @brief 构建并启动服务器
   * @return 服务器对象
   */
  std::shared_ptr<HttpServer> build();

  /**
   * @brief 构建并运行服务器（阻塞）
   */
  void run();

  /**
   * @brief 获取当前配置
   */
  const ServerConfig &config() const { return config_; }

private:
  ServerConfig config_;

  std::vector<Middleware::ptr> middlewares_;
  std::vector<std::tuple<HttpMethod, std::string, RouteHandlerWrapper>> routes_;
  RouteHandlerWrapper not_found_handler_;

  zcoroutine::IoScheduler::ptr io_scheduler_;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_BUILDER_H_
