#ifndef ZHTTP_ROUTER_ROUTER_H_
#define ZHTTP_ROUTER_ROUTER_H_

#include "zhttp/http_common.h"
#include "zhttp/router/route_handler.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace zhttp {
/** 路由匹配结果，携带处理器、路径参数及匹配类型。 */
struct RouteMatch {
    enum class MatchType { NONE, STATIC, DYNAMIC, REGEX, REDIRECT };
    bool found = false;
    HttpHandler handler;
    std::unordered_map<std::string, std::string> params;
    std::string route_id;
    MatchType match_type = MatchType::NONE;
};

/**
 * HTTP 方法与路径路由器。
 *
 * 优先匹配静态路径、动态路径，再匹配正则；freeze() 后注册与配置抛出 std::logic_error。
 */
class Router {
  public:
    /** 创建空路由表。 */
    Router();
    /** 释放路由表及持有的处理器。 */
    ~Router();
    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;
    /**
     * 注册普通路径回调。
     *
     * @param method 请求方法。
     * @param path 静态路径、:name 参数路径或 *name 通配路径。
     * @param callback 接收当前 HttpContext 的处理函数。
     */
    void add_route(HttpMethod method, const std::string &path,
                   RouterCallback callback);
    /** 注册共享所有权的路由处理器；路径规则同回调重载。 */
    void add_route(HttpMethod method, const std::string &path,
                   RouteHandler::ptr handler);
    /**
     * 注册完整路径正则路由。
     *
     * @param method 请求方法。
     * @param pattern 路径正则表达式。
     * @param params 按捕获组顺序对应的参数名称。
     * @param callback 命中时执行的回调。
     * @throws std::regex_error 正则表达式非法。
     */
    void add_regex_route(HttpMethod method, const std::string &pattern,
                         const std::vector<std::string> &params,
                         RouterCallback callback);
    /** 注册正则路由处理器，参数规则同回调重载。 */
    void add_regex_route(HttpMethod method, const std::string &pattern,
                         const std::vector<std::string> &params,
                         RouteHandler::ptr handler);
    /** 注册 GET 路径回调，路径规则同 add_route()。 */
    void get(const std::string &path, RouterCallback handler);
    /** 注册 GET 路径处理器，保持共享所有权。 */
    void get(const std::string &path, RouteHandler::ptr handler);
    /** 注册 POST 路径回调，路径规则同 add_route()。 */
    void post(const std::string &path, RouterCallback handler);
    /** 注册 POST 路径处理器，保持共享所有权。 */
    void post(const std::string &path, RouteHandler::ptr handler);
    /** 注册 PUT 路径回调，路径规则同 add_route()。 */
    void put(const std::string &path, RouterCallback handler);
    /** 注册 PUT 路径处理器，保持共享所有权。 */
    void put(const std::string &path, RouteHandler::ptr handler);
    /** 注册 DELETE 路径回调，路径规则同 add_route()。 */
    void del(const std::string &path, RouterCallback handler);
    /** 注册 DELETE 路径处理器，保持共享所有权。 */
    void del(const std::string &path, RouteHandler::ptr handler);
    /** 设置 GET/HEAD 的根路径和 /home 跳转目标。 */
    void set_homepage(const std::string &homepage);
    /** 返回归一化后的首页目标。 */
    const std::string &homepage() const { return homepage_; }
    /**
     * 按路径与方法查找处理器，不执行处理器。
     *
     * @param path 请求路径，不含查询串。
     * @param method 请求方法。
     * @return 匹配结果，未命中时 found 为 false。
     */
    RouteMatch match(const std::string &path, HttpMethod method);
    /** 冻结路由注册和首页配置。 */
    void freeze() { frozen_ = true; }

  private:
    void check_mutable() const {
        if (frozen_)
            throw std::logic_error("Router configuration is frozen");
    }
    bool is_dynamic_path(const std::string &path) const;
    bool should_redirect_to_homepage(const std::string &path,
                                     HttpMethod method) const;
    std::string normalize_homepage(const std::string &homepage) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string homepage_;
    bool frozen_ = false;
};
} // namespace zhttp

#endif // ZHTTP_ROUTER_ROUTER_H_
