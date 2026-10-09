/**
 * rate_limiter_middleware.h
 * rate_limiter_middleware 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_RATE_LIMITER_MIDDLEWARE_H_
#define ZHTTP_RATE_LIMITER_MIDDLEWARE_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/rate_limiter.h"
#include <chrono>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zhttp {
namespace mid {
/**
 * 限流中间件：基于 RateLimiter 实现
 *
 * 默认行为：
 * - key：优先使用 request.remote_addr()；若为空再尝试
 * X-Forwarded-For；最终退化为 global。
 * - limiter：默认 10 req / second 的令牌桶。
 * - 失败响应：返回 429 + 文本 body，并写入 Retry-After。
 */
class RateLimiterMiddleware : public Middleware {
  public:
    using KeyFunc = std::function<std::string(HttpContext &)>;

    /**
     * 限流中间件配置项
     */
    struct Options {
        Options() : limiter(), key_func(), retry_after_header("Retry-After") {}

        RateLimiter::ptr limiter; // 具体限流器实例；为空时使用默认令牌桶
        KeyFunc key_func;         // 从请求中提取限流维度，例如 IP、用户 ID
        std::string retry_after_header; // 被限流时写回的重试头名
    };

    /**
     * 构造限流中间件
     *
     * @param opt 限流器、中间件 key 提取规则等配置
     */
    explicit RateLimiterMiddleware(Options opt = Options());

    /**
     * 在请求进入业务前执行限流判断
     *
     * @param context 当前请求与响应上下文
     * 放行时返回 true，限流时构造响应并返回 false
     */
    bool before(HttpContext &context) override;

  private:
    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_RATE_LIMITER_MIDDLEWARE_H_
