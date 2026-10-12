#ifndef ZHTTP_HTTP_APPLICATION_H_
#define ZHTTP_HTTP_APPLICATION_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/router/router.h"
#include <memory>

namespace zhttp {

/**
 * @brief 路由与中间件的同步执行入口，不创建线程、监听器或日志器。
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

    /**
     * @brief 追加全局中间件，包含未匹配路由的请求；空实例忽略。
     *
     * before 按注册顺序执行，正常返回过 before 的实例按逆序执行 after；
     * 返回 false 会跳过后续 before 和业务处理，但仍执行已进入的 after。
     */
    void use(mid::Middleware::ptr middleware);

    /**
     * @brief 为精确请求路径追加中间件，在全局和组中间件之后执行。
     * @param path 请求路径字面值，不按动态路由模板或正则匹配。
     * @param middleware 共享持有的中间件实例；为空时忽略。
     */
    void use(const std::string &path, mid::Middleware::ptr middleware);

    /**
     * @brief 为前缀子路径追加组中间件；仅在路由命中时选择。
     *
     * @param prefix
     * 去除尾随斜杠后按目录边界匹配，仅匹配严格子路径；空或根前缀忽略。
     * @param middleware
     * 共享持有的实例；嵌套组由外到内选择，组内按注册顺序执行。
     */
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    void set_not_found_handler(HttpHandler handler);

    /**
     * @brief 设置业务与中间件异常处理器；空回调恢复默认 500 响应。
     *
     * 调用前清除原响应及升级意图；处理器再次抛出，或返回升级、流正文、1xx
     * 响应时， 回退到默认处理。
     */
    void set_exception_handler(ExceptionHandler handler);

    /**
     * @brief 冻结路由和中间件配置；可重复调用，后续注册或配置修改抛出
     * std::logic_error。
     */
    void freeze();

    /**
     * @brief 在当前线程同步处理上下文，不提交响应或触发完成通知。
     *
     * @return 路由是否命中，与中间件是否中止、最终响应是否成功无关。
     */
    bool handle(HttpContext &context) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zhttp

#endif
