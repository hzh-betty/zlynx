/**
 * @file route_handler.h
 * @brief route_handler 定义。
 * @author hzh-betty
 */

#ifndef ZHTTP_ROUTE_HANDLER_H_
#define ZHTTP_ROUTE_HANDLER_H_

#include "zhttp/http_request.h"
#include "zhttp/http_response.h"

#include <functional>
#include <memory>
#include <utility>

namespace zhttp {

/**
 * @brief 路由处理器抽象基类
 * @details
 * 框架允许两种注册业务逻辑的方式：
 * 1. 直接传入回调函数
 * 2. 继承 RouteHandler 并实现面向对象处理器
 *
 * 第二种方式更适合需要封装状态或复用逻辑的复杂处理器。
 */
class RouteHandler {
  public:
    using ptr = std::shared_ptr<RouteHandler>;

    virtual ~RouteHandler() = default;

    /**
     * @brief 处理 HTTP 请求
     * @param request HTTP 请求对象
     * @param response HTTP 响应对象
     */
    virtual void handle(const HttpRequest::ptr &request,
                        HttpResponse &response) = 0;
};

/**
 * @brief 函数式路由回调
 * @details 适用于简单的 lambda、自由函数或无状态处理逻辑。
 */
using RouterCallback =
    std::function<void(const HttpRequest::ptr &, HttpResponse &)>;

// 对象处理器仅在注册时适配，路由内部统一保存函数式回调。
inline RouterCallback make_route_callback(RouteHandler::ptr handler) {
    if (!handler) {
        return {};
    }
    return [handler = std::move(handler)](const HttpRequest::ptr &request,
                                          HttpResponse &response) {
        handler->handle(request, response);
    };
}

} // namespace zhttp

#endif // ZHTTP_ROUTE_HANDLER_H_
