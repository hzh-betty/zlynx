#ifndef ZHTTP_MIDDLEWARE_MIDDLEWARE_H_
#define ZHTTP_MIDDLEWARE_MIDDLEWARE_H_

#include "zhttp/http_context.h"
#include <memory>
namespace zhttp {
namespace mid {
/**
 * 请求处理的前置和后置钩子。
 *
 * 同一实例可被多个请求并发使用，请求状态应放入上下文或受保护的存储。
 */
class Middleware {
  public:
    using ptr = std::shared_ptr<Middleware>;
    /** 释放中间件实例。 */
    virtual ~Middleware() = default;
    // 返回 false 时跳过后续中间件和业务处理器，仍执行自身与外层的 after。
    /**
     * 在业务处理前运行。
     *
     * @return true 继续处理；false 跳过后续 before 和业务处理器，仍执行已进入的 after。
     */
    virtual bool before(HttpContext &) { return true; }
    // 对正常返回过 before 的中间件逆序调用；默认无需收尾。
    /** 对 before 正常返回的中间件逆序收尾；异常被流水线隔离。 */
    virtual void after(HttpContext &) {}
};
} // namespace mid
using Middleware = mid::Middleware;
} // namespace zhttp

#endif // ZHTTP_MIDDLEWARE_MIDDLEWARE_H_
