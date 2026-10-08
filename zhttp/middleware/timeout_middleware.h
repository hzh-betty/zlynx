/**
 * timeout_middleware.h
 * timeout_middleware 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_TIMEOUT_MIDDLEWARE_H_
#define ZHTTP_TIMEOUT_MIDDLEWARE_H_

#include "zhttp/middleware/middleware.h"
#include <chrono>

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace zhttp {
namespace mid {
/**
 * 请求处理超时中间件
 *
 * 该中间件在 before 阶段记录时间，在 after 阶段计算总耗时。
 * 若耗时超过阈值，则将响应覆写为超时响应（默认 504 Gateway Timeout）。
 */
class TimeoutMiddleware : public Middleware {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Milliseconds = std::chrono::milliseconds;
    using TimeoutHandler =
        std::function<void(HttpContext &, Milliseconds elapsed)>;

    /** 超时阈值（毫秒）、覆盖策略与自定义超时回调。 */
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

    /** 设置请求处理耗时阈值；不会抢占正在执行的业务代码。 */
    explicit TimeoutMiddleware(Options options = Options());

    /** 记录本次钩子进入时间并继续处理。 */
    bool before(HttpContext &context) override;
    /** 计算耗时并按配置改写超时响应。 */
    void after(HttpContext &context) override;

  private:
    bool should_override_response(const HttpResponse &response) const;

    Options options_;
    std::mutex mutex_;
    // 同一实例可同时注册为全局和路径中间件，计时起点按钩子进入次序保存。
    std::unordered_map<const HttpContext *, std::vector<TimePoint>> begin_times_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_TIMEOUT_MIDDLEWARE_H_
