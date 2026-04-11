#include "zhttp/mid/middleware.h"

namespace zhttp {
namespace mid {

void MiddlewareChain::add(Middleware::ptr middleware) {
    // 忽略空指针，避免后续执行阶段反复判空。
    if (middleware) {
        middlewares_.push_back(std::move(middleware));
    }
}

bool MiddlewareChain::execute_before(const HttpRequest::ptr &request,
                                     HttpResponse &response) {
    // 每次执行前都从头计数，确保链对象复用时不会带上旧状态。
    executed_count_ = 0;
    for (size_t i = 0; i < middlewares_.size(); ++i) {
        if (!middlewares_[i]->before(request, response)) {
            // 记录已经成功执行到哪里，便于 execute_after 只回调这部分中间件。
            executed_count_ = i + 1;
            return false;
        }
        executed_count_ = i + 1;
    }
    return true;
}

void MiddlewareChain::execute_after(const HttpRequest::ptr &request,
                                    HttpResponse &response) {
    // 逆序执行 after，和 before 形成栈式结构，更适合资源回收和收尾逻辑。
    for (size_t i = executed_count_; i > 0; --i) {
        middlewares_[i - 1]->after(request, response);
    }
}

} // namespace mid
} // namespace zhttp
