#include "router.h"
#include "zhttp_logger.h"

#include <utility>

namespace zhttp {

Router::Router() {
  // 默认 404 处理器保证即使用户没有显式配置，也能返回一个可读的兜底响应。
  RouterCallback default_404 = [](const HttpRequest::ptr & /*request*/,
                                  HttpResponse &response) {
    response.status(HttpStatus::NOT_FOUND)
        .content_type("text/html; charset=utf-8")
        .body("<html><body><h1>404 Not Found</h1></body></html>");
  };
  not_found_handler_ = RouteHandlerWrapper(std::move(default_404));
}

bool Router::is_dynamic_path(const std::string &path) const {
  // :id 和 *path 这两类语法都需要走动态匹配，而不是直接哈希命中。
  return path.find(':') != std::string::npos ||
         path.find('*') != std::string::npos;
}

// ========== 路由注册 ==========

void Router::add_route_internal(HttpMethod method, const std::string &path,
                                RouteHandlerWrapper wrapper) {
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

  // 正则路由虽然最终仍要做正则匹配，但先按前缀分桶可以减少候选数量。
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

void Router::use_group(const std::string &prefix, Middleware::ptr middleware) {
  if (!middleware) {
    return;
  }

  // 组前缀会先做归一化，确保 /api 和 /api/ 注册到同一个桶里。
  std::string normalized_prefix = normalize_group_prefix(prefix);
  if (normalized_prefix.empty()) {
    // 根路径组会和全局中间件语义冲突，因此直接忽略，让调用方改用 use(mw)。
    return;
  }

  group_middlewares_[normalized_prefix].push_back(std::move(middleware));
}

std::string Router::normalize_group_prefix(const std::string &prefix) const {
  if (prefix.empty()) {
    return "";
  }

  std::string normalized = prefix;
  // 统一裁掉尾随 /，避免同一个组前缀出现多份注册入口。
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }

  if (normalized == "/") {
    return "";
  }

  return normalized;
}

bool Router::is_group_prefix_match(const std::string &prefix,
                                   const std::string &path) const {
  // path 必须严格位于 prefix 之下，因此至少要多一个 "/segment"。
  if (prefix.empty() || path.size() <= prefix.size()) {
    return false;
  }

  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }

  // 目录边界校验，防止 /api 误匹配 /apiv1。
  return path[prefix.size()] == '/';
}

std::vector<Middleware::ptr>
Router::collect_group_middlewares(const std::string &path) const {
  std::vector<Middleware::ptr> middlewares;
  std::string normalized_path = path;

  // 请求路径同样做尾随 / 归一化，和注册侧保持一致。
  while (normalized_path.size() > 1 && normalized_path.back() == '/') {
    normalized_path.pop_back();
  }

  if (normalized_path.empty() || normalized_path == "/") {
    return middlewares;
  }

  for (size_t pos = 1; pos < normalized_path.size(); ++pos) {
    if (normalized_path[pos] != '/') {
      continue;
    }

    // 逐层提取候选前缀：/api/v1/users 会依次检查 /api、/api/v1。
    std::string prefix = normalized_path.substr(0, pos);
    auto it = group_middlewares_.find(prefix);
    if (it == group_middlewares_.end()) {
      continue;
    }

    if (!is_group_prefix_match(prefix, normalized_path)) {
      continue;
    }

    middlewares.insert(middlewares.end(), it->second.begin(), it->second.end());
  }

  return middlewares;
}

RouteContext Router::find_route(const std::string &path, HttpMethod method) {
  RouteContext ctx;

  ZHTTP_LOG_DEBUG("Router::find_route {} {}", method_to_string(method), path);

  // 第一层先查静态路由。绝大多数高频接口通常都是固定路径，这里最省成本。
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

  // 第二层交给基数树处理动态段和正则规则。
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
  // 先匹配路由，得到处理器、路径参数和路由级中间件信息。
  RouteContext ctx = find_route(request->path(), request->method());

  // 把匹配阶段提取出的参数回填到请求对象，后续业务代码可直接
  // request->path_param() 读取。
  for (const auto &pair : ctx.params) {
    const_cast<HttpRequest *>(request.get())
        ->set_path_param(pair.first, pair.second);
  }

  // 中间件执行顺序是：全局 -> 组前缀 -> 精确路径 -> 匹配结果附带中间件。
  MiddlewareChain chain;

  // 先追加全局中间件。
  for (const auto &mw : global_middlewares_) {
    chain.add(mw);
  }

  // 命中业务路由后，再按请求路径收集组中间件（浅层前缀优先）。
  if (ctx.found) {
    // 组中间件不参与 404 流程，这样“某前缀下的一组业务路由”语义更明确。
    std::vector<Middleware::ptr> group_middlewares =
        collect_group_middlewares(request->path());
    for (const auto &mw : group_middlewares) {
      chain.add(mw);
    }
  }

  // 再追加按路径注册的中间件。
  auto mw_it = route_middlewares_.find(request->path());
  if (mw_it != route_middlewares_.end()) {
    for (const auto &mw : mw_it->second) {
      chain.add(mw);
    }
  }

  // 最后追加路由匹配结果里自带的中间件。
  for (const auto &mw : ctx.middlewares) {
    chain.add(mw);
  }

  // before 返回 false 表示提前中断，不再进入业务处理器。
  bool should_continue = chain.execute_before(request, response);

  if (should_continue) {
    if (ctx.found) {
      // 命中路由则执行对应处理器。
      ctx.handler(request, response);
    } else {
      // 未命中则走统一的 404 处理器。
      not_found_handler_(request, response);
    }
  }

  // after 总是逆序执行，和 before 形成对称结构。
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
