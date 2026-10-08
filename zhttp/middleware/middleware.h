#pragma once
#include "zhttp/http_context.h"
#include <memory>
namespace zhttp {
namespace mid {
class Middleware {
  public:
    using ptr = std::shared_ptr<Middleware>;
    virtual ~Middleware() = default;
    // 返回 false 时跳过后续中间件和业务处理器，仍执行自身与外层的 after。
    virtual bool before(HttpContext &) { return true; }
    // 对正常返回过 before 的中间件逆序调用；默认无需收尾。
    virtual void after(HttpContext &) {}
};
} // namespace mid
using Middleware = mid::Middleware;
} // namespace zhttp
