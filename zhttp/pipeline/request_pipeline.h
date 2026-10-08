#ifndef ZHTTP_PIPELINE_REQUEST_PIPELINE_H_
#define ZHTTP_PIPELINE_REQUEST_PIPELINE_H_

#include "zhttp/middleware/middleware.h"
#include <exception>
#include "zhttp/router/router.h"
namespace zhttp {
using ExceptionHandler = std::function<void(HttpContext &, std::exception_ptr)>;
/** 请求处理流水线，组织路由、404、异常处理和中间件收尾。 */
class RequestPipeline {
  public:
    /** 构造默认 404 和 500 处理器。 */
    RequestPipeline();
    /** 追加全局中间件。 */
    void use(mid::Middleware::ptr middleware);
    /** 追加精确请求路径中间件。 */
    void use(const std::string &path, mid::Middleware::ptr middleware);
    /** 追加仅匹配前缀子路径的组中间件，根前缀忽略。 */
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    /** 同步执行处理链；返回路由是否命中，异常经配置处理器转换。 */
    bool execute(HttpContext &context, Router &router);
    /** 设置默认 404 路由处理器。 */
    void set_not_found_handler(HttpHandler callback);
    /** 设置异常处理器；空回调恢复默认处理。 */
    void set_exception_handler(ExceptionHandler handler);
    /** 返回当前异常处理器引用。 */
    const ExceptionHandler &exception_handler() const {
        return exception_handler_;
    }
    /** 冻结中间件与处理器配置，后续修改抛出 std::logic_error。 */
    void freeze() { frozen_ = true; }

  private:
    void check_mutable() const;
    // before 顺序执行，after 对已进入的中间件逆序收尾，逐个隔离异常。
    void execute_middlewares(
        HttpContext &context,
        const std::vector<mid::Middleware::ptr> &middlewares,
        const HttpHandler &handler,
        const std::function<void(std::exception_ptr)> &error) const;
    std::vector<mid::Middleware::ptr>
    collect_group_middlewares(const std::string &path) const;
    std::unordered_map<std::string, std::vector<mid::Middleware::ptr>>
        route_middlewares_, group_middlewares_;
    std::vector<mid::Middleware::ptr> global_middlewares_;
    HttpHandler not_found_handler_;
    ExceptionHandler exception_handler_;
    bool frozen_ = false;
};
} // namespace zhttp

#endif // ZHTTP_PIPELINE_REQUEST_PIPELINE_H_
