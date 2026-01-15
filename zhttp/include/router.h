#ifndef ZHTTP_ROUTER_H_
#define ZHTTP_ROUTER_H_

#include "http_request.h"
#include "http_response.h"
#include "middleware.h"
#include "radix_tree.h"
#include "route_handler.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace zhttp {

/**
 * @brief 静态路由条目（用于哈希表快速查找）
 */
struct StaticRouteEntry {
  std::unordered_map<HttpMethod, RouteHandlerWrapper> handlers;
  std::vector<Middleware::ptr> middlewares;
};

/**
 * @brief 路由匹配上下文
 */
struct RouteContext {
  bool found = false;
  RouteHandlerWrapper handler;
  std::vector<Middleware::ptr> middlewares;
  std::unordered_map<std::string, std::string> params;
};

/**
 * @brief 高性能路由器
 *
 * 三层查找机制:
 * 1. 静态路由: 哈希表 O(1) 查找
 * 2. 动态路由: 基数树，按优先级匹配 (Static > Param > CatchAll)
 * 3. 正则路由: 基数树前缀分桶 + 桶内正则匹配
 */
class Router {
public:
  Router();

  // ========== 路由注册 ==========

  /**
   * @brief 注册路由（回调函数方式）
   */
  void add_route(HttpMethod method, const std::string &path,
                 RouterCallback callback);

  /**
   * @brief 注册路由（处理器对象方式）
   */
  void add_route(HttpMethod method, const std::string &path,
                 RouteHandler::ptr handler);

  /**
   * @brief 注册正则表达式路由（回调函数方式）
   * 正则路由会按前缀分桶存储在基数树中
   */
  void add_regex_route(HttpMethod method, const std::string &regex_pattern,
                       const std::vector<std::string> &param_names,
                       RouterCallback callback);

  /**
   * @brief 注册正则表达式路由（处理器对象方式）
   */
  void add_regex_route(HttpMethod method, const std::string &regex_pattern,
                       const std::vector<std::string> &param_names,
                       RouteHandler::ptr handler);

  // ========== 便捷方法 ==========

  void get(const std::string &path, RouterCallback callback);
  void get(const std::string &path, RouteHandler::ptr handler);

  void post(const std::string &path, RouterCallback callback);
  void post(const std::string &path, RouteHandler::ptr handler);

  void put(const std::string &path, RouterCallback callback);
  void put(const std::string &path, RouteHandler::ptr handler);

  void del(const std::string &path, RouterCallback callback);
  void del(const std::string &path, RouteHandler::ptr handler);

  // ========== 中间件 ==========

  /**
   * @brief 添加全局中间件
   */
  void use(Middleware::ptr middleware);

  /**
   * @brief 为特定路由添加中间件
   */
  void use(const std::string &path, Middleware::ptr middleware);

  // ========== 路由匹配 ==========

  /**
   * @brief 路由请求
   * @return 是否找到匹配的路由
   */
  bool route(const HttpRequest::ptr &request, HttpResponse &response);

  /**
   * @brief 设置 404 处理器
   */
  void set_not_found_handler(RouterCallback callback);
  void set_not_found_handler(RouteHandler::ptr handler);

private:
  /**
   * @brief 判断路径是否包含动态参数
   */
  bool is_dynamic_path(const std::string &path) const;

  /**
   * @brief 三层查找
   * 1. 静态路由哈希表
   * 2. 基数树（动态路由 + 正则路由）
   */
  RouteContext find_route(const std::string &path, HttpMethod method);

  /**
   * @brief 注册路由内部实现
   */
  void add_route_internal(HttpMethod method, const std::string &path,
                          RouteHandlerWrapper wrapper);

  void add_regex_route_internal(HttpMethod method,
                                const std::string &regex_pattern,
                                const std::vector<std::string> &param_names,
                                RouteHandlerWrapper wrapper);

private:
  // 静态路由: 哈希表 (path -> handlers)
  std::unordered_map<std::string, StaticRouteEntry> static_routes_;

  // 动态路由 + 正则路由: 统一基数树
  RadixTree radix_tree_;

  // 路由级中间件映射
  std::unordered_map<std::string, std::vector<Middleware::ptr>>
      route_middlewares_;

  // 全局中间件
  std::vector<Middleware::ptr> global_middlewares_;

  // 404 处理器
  RouteHandlerWrapper not_found_handler_;
};

} // namespace zhttp

#endif // ZHTTP_ROUTER_H_
