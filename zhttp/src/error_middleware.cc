#include "error_middleware.h"

#include <sstream>

namespace zhttp {

ErrorMiddleware::ErrorMiddleware() = default;

ErrorMiddleware::ErrorMiddleware(const Options &options) : options_(options) {}

bool ErrorMiddleware::before(const HttpRequest::ptr &, HttpResponse &) {
    return true;
}

void ErrorMiddleware::after(const HttpRequest::ptr &request,
                            HttpResponse &response) {
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

std::string ErrorMiddleware::escape_json(const std::string &input) {
    std::string output;
    output.reserve(input.size());

    for (char ch : input) {
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output += ch;
            break;
        }
    }

    return output;
}

std::string
ErrorMiddleware::build_error_json(const HttpRequest::ptr &request,
                                  const HttpResponse &response) const {
    const int code = static_cast<int>(response.status_code());
    std::string message;
    if (code >= 500) {
        message = options_.internal_error_message;
    } else {
        message = status_to_string(response.status_code());
    }

    std::ostringstream oss;
    oss << "{\"code\":" << code << ",\"message\":\"" << escape_json(message)
        << "\"";

    if (options_.include_method) {
        oss << ",\"method\":\""
            << escape_json(method_to_string(request->method())) << "\"";
    }

    if (options_.include_path) {
        oss << ",\"path\":\"" << escape_json(request->path()) << "\"";
    }

    oss << "}";
    return oss.str();
}

} // namespace zhttp