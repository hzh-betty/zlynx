#ifndef ZHTTP_INTERNAL_REQUEST_PIPELINE_H_
#define ZHTTP_INTERNAL_REQUEST_PIPELINE_H_

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zhttp {
class HttpRequest;
class HttpResponse;
namespace mid {
class Middleware;
}
namespace detail {

// 统一管理中间件的注册、选择和执行策略。
// 路由匹配只需提供处理函数和是否命中的标志。
class RequestPipeline {
  public:
    using Callback = std::function<void(const std::shared_ptr<HttpRequest> &,
                                        HttpResponse &)>;
    using ExceptionHandler =
        std::function<void(const std::shared_ptr<HttpRequest> &, HttpResponse &,
                           std::exception_ptr)>;

    RequestPipeline();
    void use(std::shared_ptr<mid::Middleware> middleware);
    void use(const std::string &path,
             std::shared_ptr<mid::Middleware> middleware);
    void use_group(const std::string &prefix,
                   std::shared_ptr<mid::Middleware> middleware);
    bool execute(const std::shared_ptr<HttpRequest> &request,
                 HttpResponse &response, const Callback &handler, bool found);
    void set_not_found_handler(Callback callback);
    void set_exception_handler(ExceptionHandler handler);
    const ExceptionHandler &exception_handler() const {
        return exception_handler_;
    }

  private:
    std::string normalize_group_prefix(const std::string &prefix) const;
    bool is_group_prefix_match(const std::string &prefix,
                               const std::string &path) const;
    std::vector<std::shared_ptr<mid::Middleware>>
    collect_group_middlewares(const std::string &path) const;

    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<mid::Middleware>>>
        route_middlewares_;
    std::unordered_map<std::string,
                       std::vector<std::shared_ptr<mid::Middleware>>>
        group_middlewares_;
    std::vector<std::shared_ptr<mid::Middleware>> global_middlewares_;
    Callback not_found_handler_;
    ExceptionHandler exception_handler_;
};
} // 命名空间 detail
} // 命名空间 zhttp

#endif
