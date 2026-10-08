#pragma once
#include "zhttp/middleware/middleware.h"
#include <exception>
#include "zhttp/router/router.h"
namespace zhttp {
using ExceptionHandler = std::function<void(HttpContext &, std::exception_ptr)>;
class RequestPipeline {
  public:
    RequestPipeline();
    void use(mid::Middleware::ptr middleware);
    void use(const std::string &path, mid::Middleware::ptr middleware);
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    bool execute(HttpContext &context, Router &router);
    void set_not_found_handler(HttpHandler callback);
    void set_exception_handler(ExceptionHandler handler);
    const ExceptionHandler &exception_handler() const {
        return exception_handler_;
    }
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
