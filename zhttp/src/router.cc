/**
 * @file router.cc
 * @brief router 实现。
 * @author hzh-betty
 */

#include "zhttp/router.h"
#include "zhttp/zhttp_logger.h"

#include <exception>
#include <utility>

namespace zhttp {

namespace {

bool is_homepage_alias(const std::string &path) {
    return path == "/" || path == "/home" || path == "home";
}

} // namespace

Router::Router() = default;

bool Router::is_dynamic_path(const std::string &path) const {
    // :id 和 *path 这两类语法都需要走动态匹配，而不是直接哈希命中。
    return path.find(':') != std::string::npos ||
           path.find('*') != std::string::npos;
}

void Router::add_route_internal(HttpMethod method, const std::string &path,
                                RouterCallback wrapper) {
    ZHTTP_LOG_DEBUG("Router::add_route {} {}", method_to_string(method), path);

    if (is_dynamic_path(path)) {
        // 动态路由无法直接哈希命中，统一进入基数树做结构化匹配。
        radix_tree_.insert(method, path, std::move(wrapper));
        ZHTTP_LOG_DEBUG("Added to radix tree (dynamic): {}", path);
    } else {
        // 纯静态路径直接走哈希表，查询成本最低。
        static_routes_[path].handlers[method] = std::move(wrapper);
        ZHTTP_LOG_DEBUG("Added to hash map (static): {}", path);
    }
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouterCallback callback) {
    add_route_internal(method, path, std::move(callback));
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouteHandler::ptr handler) {
    add_route_internal(method, path, make_route_callback(std::move(handler)));
}

void Router::add_regex_route_internal(
    HttpMethod method, const std::string &regex_pattern,
    const std::vector<std::string> &param_names, RouterCallback wrapper) {
    ZHTTP_LOG_DEBUG("Router::add_regex_route {} {}", method_to_string(method),
                    regex_pattern);

    // 正则路由虽然最终仍要做正则匹配，但先按前缀分桶可以减少候选数量。
    radix_tree_.insert_regex(method, regex_pattern, param_names,
                             std::move(wrapper));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouterCallback callback) {
    add_regex_route_internal(method, regex_pattern, param_names,
                             std::move(callback));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouteHandler::ptr handler) {
    add_regex_route_internal(method, regex_pattern, param_names,
                             make_route_callback(std::move(handler)));
}

void Router::get(const std::string &path, RouterCallback callback) {
    add_route(HttpMethod::GET, path, std::move(callback));
}

void Router::get(const std::string &path, RouteHandler::ptr handler) {
    add_route(HttpMethod::GET, path, std::move(handler));
}

void Router::post(const std::string &path, RouterCallback callback) {
    add_route(HttpMethod::POST, path, std::move(callback));
}

void Router::post(const std::string &path, RouteHandler::ptr handler) {
    add_route(HttpMethod::POST, path, std::move(handler));
}

void Router::put(const std::string &path, RouterCallback callback) {
    add_route(HttpMethod::PUT, path, std::move(callback));
}

void Router::put(const std::string &path, RouteHandler::ptr handler) {
    add_route(HttpMethod::PUT, path, std::move(handler));
}

void Router::del(const std::string &path, RouterCallback callback) {
    add_route(HttpMethod::DELETE, path, std::move(callback));
}

void Router::del(const std::string &path, RouteHandler::ptr handler) {
    add_route(HttpMethod::DELETE, path, std::move(handler));
}

void Router::set_homepage(const std::string &homepage) {
    homepage_ = normalize_homepage(homepage);
}

void Router::use(mid::Middleware::ptr middleware) {
    pipeline_.use(std::move(middleware));
}

void Router::use(const std::string &path, mid::Middleware::ptr middleware) {
    pipeline_.use(path, std::move(middleware));
}

void Router::use_group(const std::string &prefix,
                       mid::Middleware::ptr middleware) {
    pipeline_.use_group(prefix, std::move(middleware));
}

bool Router::should_redirect_to_homepage(const std::string &path,
                                         HttpMethod method) const {
    if (homepage_.empty()) {
        return false;
    }

    if (method != HttpMethod::GET && method != HttpMethod::HEAD) {
        return false;
    }

    if (!is_homepage_alias(path)) {
        return false;
    }

    return !is_homepage_alias(homepage_);
}

std::string Router::normalize_homepage(const std::string &homepage) const {
    if (homepage.empty()) {
        return "";
    }

    if (homepage[0] == '/' || homepage.find("://") != std::string::npos) {
        return homepage;
    }

    return "/" + homepage;
}

RouteContext Router::find_route(const std::string &path, HttpMethod method) {
    RouteContext ctx;

    ZHTTP_LOG_DEBUG("Router::find_route {} {}", method_to_string(method), path);

    if (should_redirect_to_homepage(path, method)) {
        ctx.found = true;
        std::string target = homepage_;
        ctx.handler = RouterCallback(
            [target](const HttpRequest::ptr &, HttpResponse &response) {
                response.redirect(target);
            });
        ZHTTP_LOG_DEBUG("Homepage redirect {} -> {}", path, target);
        return ctx;
    }

    // 第一层先查静态路由。绝大多数高频接口通常都是固定路径，这里最省成本。
    auto static_it = static_routes_.find(path);
    if (static_it != static_routes_.end()) {
        auto handler_it = static_it->second.handlers.find(method);
        if (handler_it != static_it->second.handlers.end()) {
            ctx.found = true;
            ctx.handler = handler_it->second;
            ZHTTP_LOG_DEBUG("Found in static routes (hash map): {}", path);
            return ctx;
        }
    }

    // 第二层交给基数树处理动态段和正则规则。
    RouteMatchContext match = radix_tree_.find(path, method);
    if (match.found) {
        ctx.found = true;
        ctx.handler = match.handler;
        ctx.params = std::move(match.params);
        ZHTTP_LOG_DEBUG("Found in radix tree: {}, match_type: {}", path,
                        match.match_type ==
                                RouteMatchContext::MatchType::DYNAMIC
                            ? "DYNAMIC"
                            : "REGEX");
        return ctx;
    }

    ZHTTP_LOG_DEBUG("Route not found: {}", path);
    return ctx;
}

bool Router::route(const HttpRequest::ptr &request, HttpResponse &response) {
    // 先匹配路由，得到处理器、路径参数和路由级中间件信息。
    RouteContext ctx = find_route(request->path(), request->method());

    // 把匹配阶段提取出的参数回填到请求对象，后续业务代码可直接
    // request->path_param() 读取。
    for (const auto &pair : ctx.params) {
        const_cast<HttpRequest *>(request.get())
            ->set_path_param(pair.first, pair.second);
    }

    return pipeline_.execute(request, response, ctx.handler, ctx.found);
}

void Router::set_not_found_handler(RouterCallback callback) {
    pipeline_.set_not_found_handler(std::move(callback));
}

void Router::set_not_found_handler(RouteHandler::ptr handler) {
    pipeline_.set_not_found_handler(make_route_callback(std::move(handler)));
}

void Router::set_exception_handler(ExceptionHandler handler) {
    pipeline_.set_exception_handler(std::move(handler));
}

} // namespace zhttp
