#ifndef ZHTTP_PIPELINE_REQUEST_PIPELINE_H_
#define ZHTTP_PIPELINE_REQUEST_PIPELINE_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/router/router.h"
#include <exception>

namespace zhttp {
/**
 * @brief 请求处理流水线，组织路由、404、异常处理和中间件收尾。
 */
class RequestPipeline {
  public:
    RequestPipeline();
    void use(mid::Middleware::ptr middleware);
    void use(const std::string &path, mid::Middleware::ptr middleware);

    /**
     * @brief 追加仅匹配前缀子路径的组中间件，根前缀忽略。
     */
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);

    /**
     * @brief 同步执行处理链；返回路由是否命中，异常经配置处理器转换。
     */
    bool execute(HttpContext &context, const Router &router) const;
    void set_not_found_handler(HttpHandler callback);

    /**
     * @brief 设置异常处理器；空回调恢复默认处理。
     */
    void set_exception_handler(ExceptionHandler handler);

    /**
     * @brief 冻结中间件与处理器配置，后续修改抛出 std::logic_error。
     */
    void freeze() { frozen_ = true; }

  private:
    void check_mutable() const;

    /**
     * @brief 执行选定处理链，并为正常返回过 before 的中间件逆序收尾。
     *
     * before 抛出的当前实例不计入 after 范围；返回 false 的当前实例仍需收尾。
     * after 的异常逐个交给 error，不阻止外层中间件继续收尾。
     */
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

#endif
