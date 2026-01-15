#include "middleware.h"

namespace zhttp {

void MiddlewareChain::add(Middleware::ptr middleware) {
  if (middleware) {
    middlewares_.push_back(std::move(middleware));
  }
}

bool MiddlewareChain::execute_before(const HttpRequest::ptr &request,
                                     HttpResponse &response) {
  executed_count_ = 0;
  for (size_t i = 0; i < middlewares_.size(); ++i) {
    if (!middlewares_[i]->before(request, response)) {
      // 中间件中断了请求，记录执行位置
      executed_count_ = i + 1;
      return false;
    }
    executed_count_ = i + 1;
  }
  return true;
}

void MiddlewareChain::execute_after(const HttpRequest::ptr &request,
                                    HttpResponse &response) {
  // 逆序执行已执行过 before 的中间件的 after 方法
  for (size_t i = executed_count_; i > 0; --i) {
    middlewares_[i - 1]->after(request, response);
  }
}

} // namespace zhttp
