/**
 * router.cc
 * router 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/router/router.h"
#include "zhttp/http_context.h"
#include "zhttp/internal/radix_tree.h"
#include "zhttp/zhttp_logger.h"

#include <exception>
#include <utility>

namespace zhttp {

namespace {

bool is_homepage_alias(const std::string &path) {
    return path == "/" || path == "/home" || path == "home";
}

} // namespace

struct Router::Impl {
    using MethodHandlers = std::unordered_map<HttpMethod, RouterCallback>;
    std::unordered_map<std::string, MethodHandlers> static_routes;
    RadixTree radix_tree;
};
Router::Router() : impl_(new Impl) {}
Router::~Router() = default;

bool Router::is_dynamic_path(const std::string &path) const {
    // :id 和 *path 这两类语法都需要走动态匹配，而不是直接哈希命中。
    return path.find(':') != std::string::npos ||
           path.find('*') != std::string::npos;
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouterCallback callback) {
    check_mutable();
    ZHTTP_LOG_DEBUG("Router::add_route {} {}", method_to_string(method), path);

    if (is_dynamic_path(path)) {
        // 动态路由无法直接哈希命中，统一进入基数树做结构化匹配。
        impl_->radix_tree.insert(method, path, std::move(callback));
        ZHTTP_LOG_DEBUG("Added to radix tree (dynamic): {}", path);
    } else {
        // 纯静态路径直接走哈希表，查询成本最低。
        impl_->static_routes[path][method] = std::move(callback);
        ZHTTP_LOG_DEBUG("Added to hash map (static): {}", path);
    }
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouteHandler::ptr handler) {
    add_route(method, path, make_route_callback(std::move(handler)));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouterCallback callback) {
    check_mutable();
    ZHTTP_LOG_DEBUG("Router::add_regex_route {} {}", method_to_string(method),
                    regex_pattern);

    // 正则路由虽然最终仍要做正则匹配，但先按前缀分桶可以减少候选数量。
    impl_->radix_tree.insert_regex(method, regex_pattern, param_names,
                                   std::move(callback));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouteHandler::ptr handler) {
    add_regex_route(method, regex_pattern, param_names,
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
    check_mutable();
    homepage_ = normalize_homepage(homepage);
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

RouteMatch Router::match(const std::string &path, HttpMethod method) {
    RouteMatch ctx;
    ctx.route_id = path;

    ZHTTP_LOG_DEBUG("Router::match {} {}", method_to_string(method), path);

    if (should_redirect_to_homepage(path, method)) {
        ctx.found = true;
        std::string target = homepage_;
        ctx.match_type = RouteMatch::MatchType::REDIRECT;
        ctx.handler = RouterCallback([target](HttpContext &context) {
            auto &response = context.response();
            response.redirect(target);
        });
        ZHTTP_LOG_DEBUG("Homepage redirect {} -> {}", path, target);
        return ctx;
    }

    // 第一层先查静态路由。绝大多数高频接口通常都是固定路径，这里最省成本。
    auto static_it = impl_->static_routes.find(path);
    if (static_it != impl_->static_routes.end()) {
        auto handler_it = static_it->second.find(method);
        if (handler_it != static_it->second.end()) {
            ctx.found = true;
            ctx.handler = handler_it->second;
            ctx.match_type = RouteMatch::MatchType::STATIC;
            ZHTTP_LOG_DEBUG("Found in static routes (hash map): {}", path);
            return ctx;
        }
    }

    // 第二层交给基数树处理动态段和正则规则。
    RouteMatchContext match = impl_->radix_tree.find(path, method);
    if (match.found) {
        ctx.found = true;
        ctx.handler = match.handler;
        ctx.params = std::move(match.params);
        ctx.route_id = std::move(match.route_id);
        ctx.match_type =
            match.match_type == RouteMatchContext::MatchType::DYNAMIC
                ? RouteMatch::MatchType::DYNAMIC
                : RouteMatch::MatchType::REGEX;
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

} // namespace zhttp
