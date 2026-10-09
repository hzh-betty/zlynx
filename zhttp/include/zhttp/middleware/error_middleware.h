/**
 * error_middleware.h
 * error_middleware 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_ERROR_MIDDLEWARE_H_
#define ZHTTP_ERROR_MIDDLEWARE_H_

#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/middleware/middleware.h"

#include <string>

namespace zhttp {
namespace mid {
/**
 * 统一错误响应中间件
 *
 * 把 4xx/5xx 响应收敛为一致格式。
 * 默认只在响应体为空时填充错误体，避免覆盖业务层已经明确给出的错误信息。
 */
class ErrorMiddleware : public Middleware {
  public:
    /** 错误响应格式、覆盖条件和输出内容。 */
    struct Options {
        // 仅在当前响应体为空时才格式化错误，默认不覆盖业务层已有错误内容。
        bool only_format_when_body_empty = true;

        // 是否在错误体中包含请求方法与路径。
        bool include_method = true;
        bool include_path = true;

        // 5xx 默认输出通用错误文案，避免泄露内部细节。
        std::string internal_error_message = "Internal Server Error";
    };

    /** 使用默认错误响应格式。 */
    ErrorMiddleware();

    /** 设置错误响应格式和内容策略。 */
    explicit ErrorMiddleware(const Options &options);

    /** 按配置格式化 4xx/5xx 响应，保留无需改写的业务正文。 */
    void after(HttpContext &context) override;

  private:
  private:
    std::string build_error_json(HttpContext &request,
                                 const HttpResponse &response) const;

  private:
    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_ERROR_MIDDLEWARE_H_
