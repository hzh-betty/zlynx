/**
 * error_middleware.cc
 * error_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/error_middleware.h"

namespace zhttp {
namespace mid {

ErrorMiddleware::ErrorMiddleware() = default;

ErrorMiddleware::ErrorMiddleware(const Options &options) : options_(options) {}

void ErrorMiddleware::after(HttpContext &request) {
    auto &response = request.response();
    const int code = static_cast<int>(response.status_code());
    if (code < 400) {
        return;
    }

    // 仅在响应体为空时才格式化错误，避免覆盖业务层已有错误内容。
    if (options_.only_format_when_body_empty &&
        !response.body_content().empty()) {
        return;
    }

    response.json(build_error_json(request, response));
}

std::string
ErrorMiddleware::build_error_json(HttpContext &request,
                                  const HttpResponse &response) const {
    const int code = static_cast<int>(response.status_code());
    std::string message;
    if (code >= 500) {
        message = options_.internal_error_message;
    } else {
        message = status_to_string(response.status_code());
    }

    HttpContext::Json json = {{"code", code}, {"message", message}};

    if (options_.include_method) {
        json["method"] = method_to_string(request.method());
    }

    if (options_.include_path) {
        json["path"] = request.path();
    }

    // 错误处理路径不能因请求中的非法 UTF-8 再次抛出序列化异常。
    return json.dump(-1, ' ', false,
                     HttpContext::Json::error_handler_t::replace);
}

} // namespace mid
} // namespace zhttp
