#pragma once
#include <functional>
#include <memory>
namespace zhttp {
class HttpContext;
using HttpHandler = std::function<void(HttpContext &)>;
using RouterCallback = HttpHandler;

// 有状态业务对象的公开入口；框架通过共享所有权保持处理器存活。
// 普通函数或 lambda 可直接使用 HttpHandler，无需派生或额外包装。
class RouteHandler {
  public:
    using ptr = std::shared_ptr<RouteHandler>;
    virtual ~RouteHandler() = default;
    virtual void handle(HttpContext &context) = 0;
};
inline HttpHandler make_route_callback(RouteHandler::ptr handler) {
    if (!handler)
        return {};
    return [handler](HttpContext &context) { handler->handle(context); };
}
} // namespace zhttp
