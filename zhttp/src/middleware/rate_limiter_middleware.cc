/**
 * rate_limiter_middleware.cc
 * rate_limiter_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/rate_limiter_middleware.h"

#include <cmath>
#include <cstdint>
#include <deque>

#include "zhttp/http_request.h"

namespace zhttp {
namespace mid {

namespace {
std::string default_key(HttpContext &request) {
    // 优先使用服务器侧感知的对端地址，避免完全信任可伪造的转发头。
    // 当服务部署在反向代理之后时，再退回读取 X-Forwarded-For 的首个地址。
    // 若你的部署链路中存在多级代理，建议上层进行可信代理校验与解析。
    if (!request.remote_addr().empty()) {
        return request.remote_addr();
    }

    std::string xff = request.header("X-Forwarded-For");
    if (!xff.empty()) {
        size_t comma = xff.find(',');
        if (comma != std::string::npos) {
            xff = xff.substr(0, comma);
        }
        while (!xff.empty() && (xff.front() == ' ' || xff.front() == '\t')) {
            xff.erase(xff.begin());
        }
        while (!xff.empty() && (xff.back() == ' ' || xff.back() == '\t')) {
            xff.pop_back();
        }
        if (!xff.empty()) {
            return xff;
        }
    }

    return "global";
}


static inline int ceil_div_ms_to_s(std::chrono::milliseconds ms) {
    // HTTP Retry-After 通常使用“秒”为单位的整数；这里做向上取整。
    if (ms.count() <= 0) {
        return 0;
    }
    return static_cast<int>((ms.count() + 999) / 1000);
}

} // namespace

RateLimiterMiddleware::RateLimiterMiddleware(RateLimiterMiddleware::Options opt)
    : options_(std::move(opt)) {
    if (!options_.key_func) {
        options_.key_func = default_key;
    }
    if (!options_.limiter) {
        // 默认按来源维度做每秒 10 次的令牌桶限流。
        options_.limiter = RateLimiter::newRateLimiter(
            RateLimiter::Type::TOKEN_BUCKET, 10, RateLimiter::TimeUnit::SECOND);
    }
}

bool RateLimiterMiddleware::before(HttpContext &request) {
    auto &response = request.response();
    const std::string key = options_.key_func(request);

    if (options_.limiter->isAllowed(key)) {
        return true;
    }

    // 命中限流时直接终止后续处理，返回 429 和建议重试时间。
    response.status(HttpStatus::TOO_MANY_REQUESTS)
        .content_type("text/plain; charset=utf-8")
        .body("Too Many Requests");

    auto ra = options_.limiter->retryAfter(key);
    int retry_after = ceil_div_ms_to_s(ra);
    if (retry_after <= 0) {
        retry_after = 1;
    }
    // 即便无法准确估算，也尽量给出一个保守的最小值，便于客户端退避。
    response.header(options_.retry_after_header, std::to_string(retry_after));
    return false;
}

} // namespace mid
} // namespace zhttp
