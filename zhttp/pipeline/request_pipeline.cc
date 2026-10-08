#include "zhttp/pipeline/request_pipeline.h"
#include "zhttp/zhttp_logger.h"
#include <stdexcept>
namespace zhttp {
namespace {
void default_exception_handler(HttpContext &context, std::exception_ptr) {
    context.response()
        .status(HttpStatus::INTERNAL_SERVER_ERROR)
        .text("Internal Server Error");
}
std::string normalize_group_prefix(const std::string &prefix) {
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

bool is_group_prefix_match(const std::string &prefix,
                           const std::string &path) {
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

} // namespace
RequestPipeline::RequestPipeline()
    : not_found_handler_([](HttpContext &context) {
          context.response()
              .status(HttpStatus::NOT_FOUND)
              .html("<html><body><h1>404 Not Found</h1></body></html>");
      }),
      exception_handler_(default_exception_handler) {}
void RequestPipeline::check_mutable() const {
    if (frozen_)
        throw std::logic_error("Pipeline configuration is frozen");
}
void RequestPipeline::use_group(const std::string &prefix,
                                mid::Middleware::ptr middleware) {
    check_mutable();
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

std::vector<mid::Middleware::ptr>
RequestPipeline::collect_group_middlewares(const std::string &path) const {
    std::vector<mid::Middleware::ptr> middlewares;
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

        middlewares.insert(middlewares.end(), it->second.begin(),
                           it->second.end());
    }

    return middlewares;
}

void RequestPipeline::use(mid::Middleware::ptr middleware) {
    check_mutable();
    if (middleware)
        global_middlewares_.push_back(std::move(middleware));
}
void RequestPipeline::use(const std::string &path,
                          mid::Middleware::ptr middleware) {
    check_mutable();
    if (middleware)
        route_middlewares_[path].push_back(std::move(middleware));
}
void RequestPipeline::set_not_found_handler(HttpHandler callback) {
    check_mutable();
    not_found_handler_ = std::move(callback);
}
void RequestPipeline::set_exception_handler(ExceptionHandler handler) {
    check_mutable();
    exception_handler_ =
        handler ? std::move(handler) : default_exception_handler;
}
void RequestPipeline::execute_middlewares(
    HttpContext &context, const std::vector<mid::Middleware::ptr> &middlewares,
    const HttpHandler &handler,
    const std::function<void(std::exception_ptr)> &error) const {
    // 执行位置属于当前请求，不能保存在多个连接共享的中间件对象上。
    size_t executed = 0;
    bool proceed = true;
    try {
        for (const auto &middleware : middlewares) {
            proceed = middleware->before(context);
            ++executed;
            if (!proceed)
                break;
        }
        if (proceed && handler)
            handler(context);
    } catch (...) {
        error(std::current_exception());
    }
    while (executed) {
        try {
            middlewares[--executed]->after(context);
        } catch (...) {
            // 当前 after 失败也不能阻止外层资源清理和错误响应处理。
            error(std::current_exception());
        }
    }
}
bool RequestPipeline::execute(HttpContext &context, Router &router) {
    auto match = router.match(context.path(), context.method());
    context.set_path_params(std::move(match.params));
    // 配置列表共享只读；本次请求的选择顺序与执行状态保持局部所有权。
    std::vector<mid::Middleware::ptr> middlewares;
    for (const auto &mw : global_middlewares_)
        middlewares.push_back(mw);
    if (match.found)
        for (const auto &mw : collect_group_middlewares(context.path()))
            middlewares.push_back(mw);
    auto it = route_middlewares_.find(context.path());
    if (it != route_middlewares_.end())
        for (const auto &mw : it->second)
            middlewares.push_back(mw);
    auto error = [&](std::exception_ptr exception) {
        context.reset_result();
        try {
            exception_handler_(context, exception);
            if (context.upgrade() ||
                context.response().body_source().kind() ==
                    HttpBody::Kind::Stream ||
                context.response().status_line().status_code < 200) {
                context.reset_result();
                default_exception_handler(context, exception);
            }
        } catch (...) {
            context.reset_result();
            default_exception_handler(context, exception);
        }
    };
    execute_middlewares(context, middlewares,
                        match.found ? match.handler : not_found_handler_,
                        error);
    const auto status = context.response().status_line().status_code;
    if (context.upgrade() && status >= 200)
        context.take_upgrade();
    if ((context.upgrade() &&
         (status != 101 ||
          context.response().body_source().kind() == HttpBody::Kind::Stream ||
          context.response().body_source().length() != 0)) ||
        (!context.upgrade() && status < 200)) {
        error(std::make_exception_ptr(
            std::logic_error("Invalid processing result")));
    }
    return match.found;
}
} // namespace zhttp
