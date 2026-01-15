#include "router.h"
#include "zhttp_logger.h"

namespace zhttp {

Router::Router() {
  // 默认 404 处理器
  RouterCallback default_404 = [](const HttpRequest::ptr & /*request*/,
                                  HttpResponse &response) {
    response.status(HttpStatus::NOT_FOUND)
        .content_type("text/html; charset=utf-8")
        .body("<html><body><h1>404 Not Found</h1></body></html>");
  };
  not_found_handler_ = RouteHandlerWrapper(std::move(default_404));
}

bool Router::is_dynamic_path(const std::string &path) const {
  return path.find(':') != std::string::npos ||
         path.find('*') != std::string::npos;
}

// ========== 路由注册 ==========

void Router::add_route_internal(HttpMethod method, const std::string &path,
                                RouteHandlerWrapper wrapper) {
  ZHTTP_LOG_DEBUG("Router::add_route {} {}", method_to_string(method), path);

  if (is_dynamic_path(path)) {
    // 动态路由 -> 基数树
    radix_tree_.insert(method, path, std::move(wrapper));
    ZHTTP_LOG_DEBUG("Added to radix tree (dynamic): {}", path);
  } else {
    // 静态路由 -> 哈希表
    static_routes_[path].handlers[method] = std::move(wrapper);
    ZHTTP_LOG_DEBUG("Added to hash map (static): {}", path);
  }
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouterCallback callback) {
  add_route_internal(method, path, RouteHandlerWrapper(std::move(callback)));
}

void Router::add_route(HttpMethod method, const std::string &path,
                       RouteHandler::ptr handler) {
  add_route_internal(method, path, RouteHandlerWrapper(std::move(handler)));
}

void Router::add_regex_route_internal(
    HttpMethod method, const std::string &regex_pattern,
    const std::vector<std::string> &param_names, RouteHandlerWrapper wrapper) {
  ZHTTP_LOG_DEBUG("Router::add_regex_route {} {}", method_to_string(method),
                  regex_pattern);

  // 正则路由存入基数树（按前缀分桶）
  radix_tree_.insert_regex(method, regex_pattern, param_names,
                           std::move(wrapper));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouterCallback callback) {
  add_regex_route_internal(method, regex_pattern, param_names,
                           RouteHandlerWrapper(std::move(callback)));
}

void Router::add_regex_route(HttpMethod method,
                             const std::string &regex_pattern,
                             const std::vector<std::string> &param_names,
                             RouteHandler::ptr handler) {
  add_regex_route_internal(method, regex_pattern, param_names,
                           RouteHandlerWrapper(std::move(handler)));
}

// ========== 便捷方法 ==========

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

// ========== 中间件 ==========

void Router::use(Middleware::ptr middleware) {
  if (middleware) {
    global_middlewares_.push_back(std::move(middleware));
  }
}

void Router::use(const std::string &path, Middleware::ptr middleware) {
  if (middleware) {
    route_middlewares_[path].push_back(std::move(middleware));
  }
}

// ========== 路由匹配 ==========

RouteContext Router::find_route(const std::string &path, HttpMethod method) {
  RouteContext ctx;

  ZHTTP_LOG_DEBUG("Router::find_route {} {}", method_to_string(method), path);

  // 第一层: 静态路由哈希表查找 O(1)
  auto static_it = static_routes_.find(path);
  if (static_it != static_routes_.end()) {
    auto handler_it = static_it->second.handlers.find(method);
    if (handler_it != static_it->second.handlers.end()) {
      ctx.found = true;
      ctx.handler = handler_it->second;
      ctx.middlewares = static_it->second.middlewares;
      ZHTTP_LOG_DEBUG("Found in static routes (hash map): {}", path);
      return ctx;
    }
  }

  // 第二层: 基数树查找（动态路由 + 正则路由）
  RouteMatchContext match = radix_tree_.find(path, method);
  if (match.found) {
    ctx.found = true;
    ctx.handler = match.handler;
    ctx.params = std::move(match.params);
    ZHTTP_LOG_DEBUG("Found in radix tree: {}, match_type: {}", path,
                    match.match_type == RouteMatchContext::MatchType::DYNAMIC
                        ? "DYNAMIC"
                        : "REGEX");
    return ctx;
  }

  ZHTTP_LOG_DEBUG("Route not found: {}", path);
  return ctx;
}

bool Router::route(const HttpRequest::ptr &request, HttpResponse &response) {
  // 查找路由
  RouteContext ctx = find_route(request->path(), request->method());

  // 设置路径参数
  for (const auto &pair : ctx.params) {
    const_cast<HttpRequest *>(request.get())
        ->set_path_param(pair.first, pair.second);
  }

  // 构建中间件链
  MiddlewareChain chain;

  // 添加全局中间件
  for (const auto &mw : global_middlewares_) {
    chain.add(mw);
  }

  // 添加路由级中间件
  auto mw_it = route_middlewares_.find(request->path());
  if (mw_it != route_middlewares_.end()) {
    for (const auto &mw : mw_it->second) {
      chain.add(mw);
    }
  }

  // 添加匹配到的路由中间件
  for (const auto &mw : ctx.middlewares) {
    chain.add(mw);
  }

  // 执行 before 中间件
  bool should_continue = chain.execute_before(request, response);

  if (should_continue) {
    if (ctx.found) {
      // 执行处理器
      ctx.handler(request, response);
    } else {
      // 404 处理
      not_found_handler_(request, response);
    }
  }

  // 执行 after 中间件（逆序）
  chain.execute_after(request, response);

  return ctx.found;
}

void Router::set_not_found_handler(RouterCallback callback) {
  not_found_handler_ = RouteHandlerWrapper(std::move(callback));
}

void Router::set_not_found_handler(RouteHandler::ptr handler) {
  not_found_handler_ = RouteHandlerWrapper(std::move(handler));
}

} // namespace zhttp
