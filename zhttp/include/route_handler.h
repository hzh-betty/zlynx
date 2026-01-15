#ifndef ZHTTP_ROUTE_HANDLER_H_
#define ZHTTP_ROUTE_HANDLER_H_

#include "http_request.h"
#include "http_response.h"

#include <functional>
#include <memory>

namespace zhttp {

/**
 * @brief 路由处理器抽象基类
 * 用户可以继承此类实现自定义路由处理逻辑
 */
class RouteHandler {
public:
  using ptr = std::shared_ptr<RouteHandler>;

  virtual ~RouteHandler() = default;

  /**
   * @brief 处理HTTP请求
   * @param request HTTP请求对象
   * @param response HTTP响应对象
   */
  virtual void handle(const HttpRequest::ptr &request,
                      HttpResponse &response) = 0;
};

/**
 * @brief 函数式路由回调
 * 用于简单的lambda或函数指针场景
 */
using RouterCallback =
    std::function<void(const HttpRequest::ptr &, HttpResponse &)>;

} // namespace zhttp

#endif // ZHTTP_ROUTE_HANDLER_H_
