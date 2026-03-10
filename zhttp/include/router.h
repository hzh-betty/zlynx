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
 * @details
 * 对于完全固定的路径，例如 /health、/login，路由器会直接放进哈希表，
 * 这样匹配时只需要按路径做 O(1) 查找。
 */
struct StaticRouteEntry {
  std::unordered_map<HttpMethod, RouteHandlerWrapper> handlers;
  std::vector<Middleware::ptr> middlewares;
};

/**
 * @brief 路由匹配上下文
 * @details
 * find_route() 不直接执行处理器，而是把匹配结果先包装成上下文，供 route()
 * 后续统一组装参数、中间件链并执行。
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

  /**
   * @brief 注册路由（回调函数方式）
    * @param method HTTP 方法
    * @param path 路由路径，可以是静态路径或动态路径
    * @param callback 处理函数
   */
  void add_route(HttpMethod method, const std::string &path,
                 RouterCallback callback);

  /**
   * @brief 注册路由（处理器对象方式）
    * @param method HTTP 方法
    * @param path 路由路径
    * @param handler 处理器对象
   */
  void add_route(HttpMethod method, const std::string &path,
                 RouteHandler::ptr handler);

  /**
   * @brief 注册正则表达式路由（回调函数方式）
    * @param method HTTP 方法
    * @param regex_pattern 正则表达式模式
    * @param param_names 正则捕获组对应的参数名列表
    * @param callback 处理函数
    * @details 正则路由会按前缀分桶存储在基数树中，减少全量正则扫描的成本。
   */
  void add_regex_route(HttpMethod method, const std::string &regex_pattern,
                       const std::vector<std::string> &param_names,
                       RouterCallback callback);

  /**
   * @brief 注册正则表达式路由（处理器对象方式）
    * @param method HTTP 方法
    * @param regex_pattern 正则表达式模式
    * @param param_names 正则捕获组对应的参数名列表
    * @param handler 处理器对象
   */
  void add_regex_route(HttpMethod method, const std::string &regex_pattern,
                       const std::vector<std::string> &param_names,
                       RouteHandler::ptr handler);

  /**
   * @brief 注册 GET 路由（回调函数）
   * @param path 路由路径
   * @param callback 处理函数
   */
  void get(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册 GET 路由（处理器对象）
   * @param path 路由路径
   * @param handler 处理器对象
   */
  void get(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册 POST 路由（回调函数）
   * @param path 路由路径
   * @param callback 处理函数
   */
  void post(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册 POST 路由（处理器对象）
   * @param path 路由路径
   * @param handler 处理器对象
   */
  void post(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册 PUT 路由（回调函数）
   * @param path 路由路径
   * @param callback 处理函数
   */
  void put(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册 PUT 路由（处理器对象）
   * @param path 路由路径
   * @param handler 处理器对象
   */
  void put(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 注册 DELETE 路由（回调函数）
   * @param path 路由路径
   * @param callback 处理函数
   */
  void del(const std::string &path, RouterCallback callback);

  /**
   * @brief 注册 DELETE 路由（处理器对象）
   * @param path 路由路径
   * @param handler 处理器对象
   */
  void del(const std::string &path, RouteHandler::ptr handler);

  /**
   * @brief 设置首页跳转目标
   * @param homepage 首页目标路径或绝对 URL
   * @details
   * 设置后，访问 /、/home 或 home 的 GET/HEAD 请求会自动重定向到该目标。
   * 若传入的是不带前导 / 的站内路径，例如 dashboard，会自动规范化为 /dashboard。
   */
  void set_homepage(const std::string &homepage);

  /**
   * @brief 获取当前首页跳转目标
   * @return 当前已配置的目标；空串表示未启用
   */
  const std::string &homepage() const { return homepage_; }

  /**
   * @brief 添加全局中间件
    * @param middleware 中间件对象
   */
  void use(Middleware::ptr middleware);

  /**
   * @brief 为特定路由添加中间件
    * @param path 目标路由路径
    * @param middleware 中间件对象
   */
  void use(const std::string &path, Middleware::ptr middleware);

  /**
   * @brief 为特定前缀路由组添加中间件
    * @param prefix 路由组前缀，例如 /api
    * @param middleware 中间件对象
    * @details
    * 这是“路由组”语义，而不是精确路径语义：
    * 1. 只匹配 prefix 的子路由，例如 /api 会作用于 /api/users
    * 2. 不作用于 prefix 自身，例如 /api 不会作用于 /api
    * 3. 只按目录边界匹配，避免 /api 误命中 /apiv1
    * 4. 仅在请求最终命中业务路由时执行，404 不会触发该组中间件
   */
  void use_group(const std::string &prefix, Middleware::ptr middleware);


  /**
    * @brief 路由请求并执行中间件链
    * @param request HTTP 请求对象
    * @param response HTTP 响应对象
    * @return true 表示命中了业务路由，false 表示走了 404 处理器
   */
  bool route(const HttpRequest::ptr &request, HttpResponse &response);

  /**
   * @brief 设置 404 处理器
    * @param callback 自定义 404 回调
   */
  void set_not_found_handler(RouterCallback callback);

    /**
    * @brief 设置 404 处理器
    * @param handler 自定义 404 处理器对象
    */
  void set_not_found_handler(RouteHandler::ptr handler);

private:
  /**
   * @brief 判断路径是否包含动态参数
    * @param path 路由路径
    * @return true 表示包含 :param 或 *catch-all 语法
   */
  bool is_dynamic_path(const std::string &path) const;

  /**
   * @brief 三层查找
   * @param path 请求路径
   * @param method 请求方法
   * @return 路由匹配上下文
   * @details
   * 先查静态哈希表，再查基数树中的动态/正则路由，这样兼顾静态路径性能和
   * 动态规则的表达能力。
   */
  RouteContext find_route(const std::string &path, HttpMethod method);

  /**
   * @brief 注册路由内部实现
   * @param method HTTP 方法
   * @param path 路由路径
   * @param wrapper 已统一包装好的处理器
   */
  void add_route_internal(HttpMethod method, const std::string &path,
                          RouteHandlerWrapper wrapper);

  /**
   * @brief 注册正则路由内部实现
   * @param method HTTP 方法
   * @param regex_pattern 正则表达式模式
   * @param param_names 捕获组参数名
   * @param wrapper 已统一包装好的处理器
   */
  void add_regex_route_internal(HttpMethod method,
                                const std::string &regex_pattern,
                                const std::vector<std::string> &param_names,
                                RouteHandlerWrapper wrapper);

  /**
   * @brief 规范化路由组前缀
   * @param prefix 原始前缀
    * @return 规范化后的前缀；若为空或等价于根路径则返回空字符串
    * @details
    * 当前实现把空串和 / 都视为“无效组前缀”，避免和全局中间件语义重叠。
    * 同时会移除末尾多余的 /，让 /api 和 /api/ 归一到同一组。
   */
  std::string normalize_group_prefix(const std::string &prefix) const;

  /**
   * @brief 判断前缀是否严格匹配某个子路由路径
   * @param prefix 组前缀
   * @param path 请求路径
    * @return true 表示 prefix 仅作为 path 的父级前缀命中
    * @details
    * 这里要求 path 比 prefix 更长，且 prefix 后面紧跟目录分隔符 /。
    * 因此 /api 可以匹配 /api/users，但不会匹配 /api 或 /apiv1/users。
   */
  bool is_group_prefix_match(const std::string &prefix,
                             const std::string &path) const;

  /**
   * @brief 收集请求路径对应的组中间件
   * @param path 请求路径
   * @return 按前缀深度从浅到深排列的中间件列表
    * @details
    * 例如请求 /api/v1/users 时，会依次尝试 /api 和 /api/v1。
    * 返回结果保持“浅层组在前、深层组在后”，这样 after 阶段会自然按深到浅回退。
   */
  std::vector<Middleware::ptr>
  collect_group_middlewares(const std::string &path) const;

  /**
   * @brief 判断当前请求是否应该触发首页跳转
   */
  bool should_redirect_to_homepage(const std::string &path,
                                   HttpMethod method) const;

  /**
   * @brief 规范化首页跳转目标
   */
  std::string normalize_homepage(const std::string &homepage) const;

private:
  // 静态路由：路径完全匹配时直接命中哈希表。
  std::unordered_map<std::string, StaticRouteEntry> static_routes_;

  // 动态路由和正则路由统一交给基数树管理。
  RadixTree radix_tree_;

  // 路由级中间件映射，key 是注册时的原始路径。
  std::unordered_map<std::string, std::vector<Middleware::ptr>>
      route_middlewares_;

  // 前缀路由组中间件，key 是规范化后的静态前缀。
  // 这里不存动态模式或正则，避免把“组匹配”和“路由匹配”耦合到一起。
  std::unordered_map<std::string, std::vector<Middleware::ptr>>
      group_middlewares_;

  // 全局中间件，对每个请求都生效。
  std::vector<Middleware::ptr> global_middlewares_;

  // 路由未命中时使用的兜底处理器。
  RouteHandlerWrapper not_found_handler_;

  // 访问 / 或 /home 时的跳转目标；空串表示关闭该功能。
  std::string homepage_;
};

} // namespace zhttp

#endif // ZHTTP_ROUTER_H_
