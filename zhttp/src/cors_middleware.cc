#include "cors_middleware.h"

#include "http_common.h"

namespace zhttp {

namespace {

static const char *kHeaderOrigin = "Origin";
static const char *kHeaderReqMethod = "Access-Control-Request-Method";
static const char *kHeaderReqHeaders = "Access-Control-Request-Headers";

static const char *kHeaderAllowOrigin = "Access-Control-Allow-Origin";
static const char *kHeaderAllowMethods = "Access-Control-Allow-Methods";
static const char *kHeaderAllowHeaders = "Access-Control-Allow-Headers";
static const char *kHeaderAllowCredentials = "Access-Control-Allow-Credentials";
static const char *kHeaderExposeHeaders = "Access-Control-Expose-Headers";
static const char *kHeaderMaxAge = "Access-Control-Max-Age";

} // namespace

CorsMiddleware::CorsMiddleware(CorsMiddleware::Options options)
    : options_(std::move(options)) {}

bool CorsMiddleware::is_preflight_request(
    const HttpRequest::ptr &request) const {
    // 预检请求的最小判定：
    // - 方法是 OPTIONS；
    // - 携带 Origin；
    // - 携带 Access-Control-Request-Method。
    if (request->method() != HttpMethod::OPTIONS) {
        return false;
    }
    return !request->header(kHeaderOrigin).empty() &&
           !request->header(kHeaderReqMethod).empty();
}

bool CorsMiddleware::is_origin_allowed(const std::string &origin) const {
    if (origin.empty()) {
        return true;
    }

    for (const auto &allowed : options_.allow_origins) {
        if (allowed == "*" || allowed == origin) {
            return true;
        }
    }
    return false;
}

std::string
CorsMiddleware::resolve_allow_origin(const std::string &origin) const {
    // - 请求里带 Origin 时优先回显；
    // - 请求里无 Origin 时回退为 "*"。
    if (origin.empty()) {
        return "*";
    }
    return origin;
}

void CorsMiddleware::append_vary_value(HttpResponse &response,
                                       const std::string &value) const {
    if (value.empty()) {
        return;
    }

    auto it = response.headers().find("Vary");
    if (it == response.headers().end()) {
        response.header("Vary", value);
        return;
    }

    // 避免重复拼接相同 Vary token（大小写不敏感比较）。
    std::string existing = it->second;
    std::vector<std::string> tokens = split_string(existing, ',');
    const std::string target = to_lower(value);
    for (auto &token : tokens) {
        trim(token);
        if (to_lower(token) == target) {
            return;
        }
    }

    existing += ", ";
    existing += value;
    response.header("Vary", existing);
}

void CorsMiddleware::apply_common_cors_headers(const HttpRequest::ptr &request,
                                               HttpResponse &response) const {
    const std::string origin = request->header(kHeaderOrigin);
    if (!is_origin_allowed(origin)) {
        return;
    }

    const std::string allow_origin = resolve_allow_origin(origin);
    response.header(kHeaderAllowOrigin, allow_origin);

    if (options_.allow_credentials) {
        response.header(kHeaderAllowCredentials, "true");
    }

    if (!options_.expose_headers.empty()) {
        response.header(kHeaderExposeHeaders,
                        join_string(options_.expose_headers, ", "));
    }

    // 当返回值依赖请求 Origin 时，建议显式声明 Vary: Origin，
    // 防止 CDN/代理把某个 Origin 的响应复用给其它 Origin。
    if (options_.add_vary_origin && allow_origin != "*") {
        append_vary_value(response, "Origin");
    }
}

bool CorsMiddleware::before(const HttpRequest::ptr &request,
                            HttpResponse &response) {
    // 非预检请求不在 before 阶段处理，交给业务链路继续执行，
    // 最终在 after 阶段统一补齐 CORS 响应头。
    if (!is_preflight_request(request)) {
        return true;
    }

    const std::string origin = request->header(kHeaderOrigin);
    if (!is_origin_allowed(origin) &&
        options_.forbid_disallowed_origin_on_preflight) {
        response.status(HttpStatus::FORBIDDEN).text("Forbidden");
        return false;
    }

    // 预检响应：
    // - 先复用普通 CORS 头逻辑；
    // - 再补预检特有头（Allow-Methods / Allow-Headers / Max-Age）。
    // 这样可确保预检和实际请求在 Origin/Credentials 等关键头上的行为一致。
    apply_common_cors_headers(request, response);

    const std::string allow_methods = join_string(options_.allow_methods, ", ");
    if (!allow_methods.empty()) {
        response.header(kHeaderAllowMethods, allow_methods);
        response.header("Allow", allow_methods);
    }

    if (!options_.allow_headers.empty()) {
        response.header(kHeaderAllowHeaders,
                        join_string(options_.allow_headers, ", "));
    } else {
        // 未配置固定 allow_headers 时，按请求中的
        // Access-Control-Request-Headers 原样回显。
        // 该策略更接近 drogon 的常见用法，兼顾灵活性与易用性。
        const std::string req_headers = request->header(kHeaderReqHeaders);
        if (!req_headers.empty()) {
            response.header(kHeaderAllowHeaders, req_headers);
        }
    }

    if (options_.max_age > 0) {
        response.header(kHeaderMaxAge, std::to_string(options_.max_age));
    }

    if (!options_.short_circuit_preflight) {
        // 若关闭短路，预检请求会继续进入后续路由，
        // 方便用户在应用层自定义 OPTIONS 响应体。
        return true;
    }

    // 预检短路时直接返回 204（无响应体）。
    response.status(HttpStatus::NO_CONTENT).body("");
    return false;
}

void CorsMiddleware::after(const HttpRequest::ptr &request,
                           HttpResponse &response) {
    // 普通请求阶段补齐 CORS 头；预检请求即便 short-circuit，
    // 也会执行到 after，这里重复设置是幂等的（同名头会覆盖为同值）。
    apply_common_cors_headers(request, response);
}

} // namespace zhttp
