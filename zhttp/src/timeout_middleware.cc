#include "timeout_middleware.h"

namespace zhttp {

TimeoutMiddleware::TimeoutMiddleware(TimeoutMiddleware::Options options)
    : options_(std::move(options)) {}

bool TimeoutMiddleware::before(const HttpRequest::ptr &request,
                               HttpResponse &) {
    std::lock_guard<std::mutex> lock(mutex_);
    begin_times_[request.get()] = TimerHelper::steady_now();
    return true;
}

void TimeoutMiddleware::after(const HttpRequest::ptr &request,
                              HttpResponse &response) {
    TimePoint begin;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = begin_times_.find(request.get());
        if (it != begin_times_.end()) {
            begin = it->second;
            begin_times_.erase(it);
            found = true;
        }
    }

    if (!found) {
        return;
    }

    if (options_.timeout_ms == 0) {
        return;
    }

    const auto elapsed =
        TimerHelper::to_milliseconds(TimerHelper::steady_now() - begin);

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
        options_.timeout_handler(request, response, elapsed);
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

} // namespace zhttp