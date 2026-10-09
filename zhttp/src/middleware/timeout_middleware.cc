/**
 * timeout_middleware.cc
 * timeout_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/timeout_middleware.h"
#include <algorithm>
#include <iterator>

namespace zhttp {
namespace mid {

TimeoutMiddleware::TimeoutMiddleware(TimeoutMiddleware::Options options)
    : options_(std::move(options)) {}

bool TimeoutMiddleware::before(HttpContext &context) {
    context.timeout_entries_.emplace_back(this, std::chrono::steady_clock::now());
    return true;
}

void TimeoutMiddleware::after(HttpContext &context) {
    auto &entries = context.timeout_entries_;
    auto it = std::find_if(entries.rbegin(), entries.rend(), [this](const auto &entry) {
        return entry.first == this;
    });
    if (it == entries.rend())
        return;
    const auto begin = it->second;
    entries.erase(std::next(it).base());
    auto &request = context;
    auto &response = context.response();
    if (options_.timeout_ms == 0) {
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    // 若未超时则直接返回，避免不必要的响应覆写逻辑。
    if (elapsed.count() <= static_cast<int64_t>(options_.timeout_ms)) {
        return;
    }

    // 默认仅在当前响应不是 4xx/5xx 时才覆写，避免吞掉已有业务错误。
    if (!should_override_response(response)) {
        return;
    }

    // 执行自定义超时处理（如果配置了），优先于默认的
    // timeout_status/timeout_body。
    if (options_.timeout_handler) {
        options_.timeout_handler(request, elapsed);
        return;
    }

    response.status(options_.timeout_status)
        .content_type("text/plain; charset=utf-8")
        .body(options_.timeout_body);
}

bool TimeoutMiddleware::should_override_response(
    const HttpResponse &response) const {
    if (!options_.override_non_error_only) {
        return true;
    }

    const int code = static_cast<int>(response.status_code());
    return code < 400;
}

} // namespace mid
} // namespace zhttp
