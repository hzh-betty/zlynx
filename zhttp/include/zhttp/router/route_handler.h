#ifndef ZHTTP_ROUTER_ROUTE_HANDLER_H_
#define ZHTTP_ROUTER_ROUTE_HANDLER_H_

#include <functional>
#include <exception>
#include <memory>
namespace zhttp {
class HttpContext;
/** 同步 HTTP 业务回调，参数引用仅在本次请求处理期间有效。 */
using HttpHandler = std::function<void(HttpContext &)>;
using ExceptionHandler = std::function<void(HttpContext &, std::exception_ptr)>;
/** 路由回调的兼容类型名。 */
using RouterCallback = HttpHandler;

// 有状态业务对象的公开入口；框架通过共享所有权保持处理器存活。
// 普通函数或 lambda 可直接使用 HttpHandler，无需派生或额外包装。
/** 有状态路由处理器基类；实例由框架共享持有，handle 可能并发调用。 */
class RouteHandler {
  public:
    using ptr = std::shared_ptr<RouteHandler>;
    /** 释放业务处理器。 */
    virtual ~RouteHandler() = default;
    /**
     * 同步处理本次请求并构造响应。
     *
     * @param context 当前上下文，不得保留引用用于请求结束后处理。
     */
    virtual void handle(HttpContext &context) = 0;
};
/** 将共享处理器包装为回调；空处理器返回空回调。 */
inline HttpHandler make_route_callback(RouteHandler::ptr handler) {
    if (!handler)
        return {};
    return [handler](HttpContext &context) { handler->handle(context); };
}
} // namespace zhttp

#endif // ZHTTP_ROUTER_ROUTE_HANDLER_H_
