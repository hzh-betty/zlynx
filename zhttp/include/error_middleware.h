#ifndef ZHTTP_ERROR_MIDDLEWARE_H_
#define ZHTTP_ERROR_MIDDLEWARE_H_

#include "http_request.h"
#include "http_response.h"
#include "middleware.h"

#include <string>

namespace zhttp {

/**
 * @brief 统一错误响应中间件
 * @details
 * 把 4xx/5xx 响应收敛为一致格式。
 * 默认只在响应体为空时填充错误体，避免覆盖业务层已经明确给出的错误信息。
 */
class ErrorMiddleware : public Middleware {
public:
  struct Options {
    // 仅在当前响应体为空时才格式化错误，默认不覆盖业务层已有错误内容。
    bool only_format_when_body_empty = true;

    // 是否在错误体中包含请求方法与路径。
    bool include_method = true;
    bool include_path = true;

    // 5xx 默认输出通用错误文案，避免泄露内部细节。
    std::string internal_error_message = "Internal Server Error";
  };

  ErrorMiddleware();

  explicit ErrorMiddleware(const Options &options);

  bool before(const HttpRequest::ptr &request, HttpResponse &response) override;

  void after(const HttpRequest::ptr &request, HttpResponse &response) override;

private:
  /**
  * @brief JSON 字符串转义
   * @param input 原始字符串
   * @return 转义后的字符串，适合直接嵌入 JSON 文本
  */
  static std::string escape_json(const std::string &input);

  std::string build_error_json(const HttpRequest::ptr &request,
                               const HttpResponse &response) const;

private:
  Options options_;
};

} // namespace zhttp

#endif // ZHTTP_ERROR_MIDDLEWARE_H_