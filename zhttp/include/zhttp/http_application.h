#ifndef ZHTTP_HTTP_APPLICATION_H_
#define ZHTTP_HTTP_APPLICATION_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/router/router.h"
#include <memory>

namespace zhttp {

/** 路由与中间件的同步执行入口，不创建线程、监听器或日志器。
 * 注册须在 freeze() 前完成；冻结后可并发处理不同上下文。
 * 处理器内部共享状态仍由调用方负责同步。完成通知由最终写出方负责。
 */
class HttpApplication {
  public:
    HttpApplication();
    ~HttpApplication();
    HttpApplication(const HttpApplication &) = delete;
    HttpApplication &operator=(const HttpApplication &) = delete;
    Router &router();
    const Router &router() const;
    void use(mid::Middleware::ptr middleware);
    void use(const std::string &path, mid::Middleware::ptr middleware);
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    void set_not_found_handler(HttpHandler handler);
    void set_exception_handler(ExceptionHandler handler);
    void freeze();
    /** 返回路由是否命中；不会提交响应或触发完成通知。 */
    bool handle(HttpContext &context) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zhttp

#endif
