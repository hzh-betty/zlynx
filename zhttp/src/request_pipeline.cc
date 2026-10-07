#include "zhttp/internal/request_pipeline.h"

#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/mid/middleware.h"
#include "zhttp/zhttp_logger.h"
#include <utility>

namespace zhttp {
namespace detail {
namespace {
void set_internal_server_error(HttpResponse &response) {
    response.status(HttpStatus::INTERNAL_SERVER_ERROR)
        .content_type("text/plain; charset=utf-8")
        .body("Internal Server Error");
}

void default_exception_handler(const HttpRequest::ptr &request,
                               HttpResponse &response,
                               std::exception_ptr exception) {
    try {
        if (exception) {
            std::rethrow_exception(exception);
        }
    } catch (const std::exception &ex) {
        ZHTTP_LOG_ERROR("Unhandled exception in {} {}: {}",
                        method_to_string(request->method()), request->path(),
                        ex.what());
    } catch (...) {
        ZHTTP_LOG_ERROR("Unhandled non-std exception in {} {}",
                        method_to_string(request->method()), request->path());
    }

    set_internal_server_error(response);
}

void invoke_exception_handler(const RequestPipeline::ExceptionHandler &handler,
                              const HttpRequest::ptr &request,
                              HttpResponse &response,
                              std::exception_ptr exception) {
    try {
        handler(request, response, exception);
    } catch (const std::exception &ex) {
        ZHTTP_LOG_ERROR("Exception handler threw in {} {}: {}",
                        method_to_string(request->method()), request->path(),
                        ex.what());
        set_internal_server_error(response);
    } catch (...) {
        ZHTTP_LOG_ERROR("Exception handler threw non-std exception in {} {}",
                        method_to_string(request->method()), request->path());
        set_internal_server_error(response);
    }
}

} // 命名空间

RequestPipeline::RequestPipeline() {
    // 默认 404 处理器保证即使用户没有显式配置，也能返回一个可读的兜底响应。
    Callback default_404 = [](const HttpRequest::ptr & /*request*/,
                              HttpResponse &response) {
        response.status(HttpStatus::NOT_FOUND)
            .content_type("text/html; charset=utf-8")
            .body("<html><body><h1>404 Not Found</h1></body></html>");
    };
    not_found_handler_ = std::move(default_404);
    exception_handler_ = default_exception_handler;
}

void RequestPipeline::use_group(const std::string &prefix,
                                mid::Middleware::ptr middleware) {
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

std::string
RequestPipeline::normalize_group_prefix(const std::string &prefix) const {
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

bool RequestPipeline::is_group_prefix_match(const std::string &prefix,
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

void RequestPipeline::set_exception_handler(ExceptionHandler handler) {
    if (handler) {
        exception_handler_ = std::move(handler);
    } else {
        exception_handler_ = default_exception_handler;
    }
}

void RequestPipeline::use(mid::Middleware::ptr middleware) {
    if (middleware) {
        global_middlewares_.push_back(std::move(middleware));
    }
}

void RequestPipeline::use(const std::string &path,
                          mid::Middleware::ptr middleware) {
    if (middleware) {
        route_middlewares_[path].push_back(std::move(middleware));
    }
}

void RequestPipeline::set_not_found_handler(Callback callback) {
    not_found_handler_ = std::move(callback);
}

bool RequestPipeline::execute(const HttpRequest::ptr &request,
                              HttpResponse &response, const Callback &handler,
                              bool found) {
    // 中间件执行顺序是：全局 -> 组前缀 -> 精确路径。
    mid::MiddlewareChain chain;

    // 先追加全局中间件。
    for (const auto &mw : global_middlewares_) {
        chain.add(mw);
    }

    // 命中业务路由后，再按请求路径收集组中间件（浅层前缀优先）。
    if (found) {
        // 组中间件不参与 404 流程，这样“某前缀下的一组业务路由”语义更明确。
        std::vector<mid::Middleware::ptr> group_middlewares =
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

    // before 返回 false 表示提前中断，不再进入业务处理器。
    bool should_continue = false;
    bool has_exception = false;

    try {
        should_continue = chain.execute_before(request, response);
    } catch (...) {
        has_exception = true;
        invoke_exception_handler(exception_handler_, request, response,
                                 std::current_exception());
    }

    if (!has_exception && should_continue) {
        try {
            if (found) {
                // 命中路由则执行对应处理器。
                if (handler) {
                    handler(request, response);
                }
            } else {
                // 未命中则走统一的 404 处理器。
                if (not_found_handler_) {
                    not_found_handler_(request, response);
                }
            }
        } catch (...) {
            has_exception = true;
            invoke_exception_handler(exception_handler_, request, response,
                                     std::current_exception());
        }
    }

    // after 总是逆序执行，和 before 形成对称结构。
    try {
        chain.execute_after(request, response);
    } catch (...) {
        invoke_exception_handler(exception_handler_, request, response,
                                 std::current_exception());
    }

    return found;
}

} // 命名空间 detail
} // 命名空间 zhttp
