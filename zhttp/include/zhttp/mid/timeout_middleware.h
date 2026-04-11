#ifndef ZHTTP_TIMEOUT_MIDDLEWARE_H_
#define ZHTTP_TIMEOUT_MIDDLEWARE_H_

#include "zhttp/internal/http_utils.h"
#include "zhttp/mid/middleware.h"

#include <functional>
#include <mutex>
#include <unordered_map>

namespace zhttp {
namespace mid {
/**
 * @brief 请求处理超时中间件
 * @details
 * 该中间件在 before 阶段记录请求进入时间，在 after 阶段计算总耗时。
 * 若耗时超过阈值，则将响应覆写为超时响应（默认 504 Gateway Timeout）。
 */
class TimeoutMiddleware : public Middleware {
  public:
    using TimePoint = TimerHelper::SteadyTimePoint;
    using Milliseconds = TimerHelper::Milliseconds;
    using TimeoutHandler = std::function<void(
        const HttpRequest::ptr &, HttpResponse &, Milliseconds elapsed)>;

    struct Options {
        Options()
            : timeout_ms(1000), override_non_error_only(true),
              timeout_status(HttpStatus::GATEWAY_TIMEOUT),
              timeout_body("Gateway Timeout") {}

        uint64_t timeout_ms;

        // 默认仅在当前响应不是 4xx/5xx 时覆写，避免吞掉已有业务错误。
        bool override_non_error_only;

        HttpStatus timeout_status;
        std::string timeout_body;

        // 自定义超时处理，设置后优先于 timeout_status/timeout_body。
        TimeoutHandler timeout_handler;
    };

    explicit TimeoutMiddleware(Options options = Options());

    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    /**
     * @brief 判断是否应该覆写响应
     * @param response 响应对象
     * @return true 表示应该覆写，false 表示不应该覆写
     */
    bool should_override_response(const HttpResponse &response) const;

    Options options_;
    std::mutex mutex_;
    std::unordered_map<const HttpRequest *, TimePoint> begin_times_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_TIMEOUT_MIDDLEWARE_H_
