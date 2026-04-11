#ifndef ZHTTP_REQUEST_BODY_MIDDLEWARE_H_
#define ZHTTP_REQUEST_BODY_MIDDLEWARE_H_

#include "middleware.h"

#include <string>

namespace zhttp {

/**
 * @brief 请求体解析中间件
 * @details
 * 参考 drogon 的请求体访问体验：在进入业务处理器前，按 Content-Type
 * 自动触发请求体解析，让业务代码可直接读取 JSON、表单和上传文件数据。
 */
class RequestBodyMiddleware : public Middleware {
  public:
    struct Options {
        Options()
            : parse_json(true), parse_form_urlencoded(true),
              parse_multipart(true), reject_invalid_json(true),
              reject_invalid_multipart(true),
              invalid_json_message("Invalid JSON body"),
              invalid_multipart_message("Invalid multipart/form-data body") {}

        // 自动解析各类请求体。
        bool parse_json;
        bool parse_form_urlencoded;
        bool parse_multipart;

        // 解析失败时是否直接返回 400 并中断后续处理。
        bool reject_invalid_json;
        bool reject_invalid_multipart;

        // 错误响应文案。
        std::string invalid_json_message;
        std::string invalid_multipart_message;
    };

    explicit RequestBodyMiddleware(Options options = Options());

    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    Options options_;
};

} // namespace zhttp

#endif // ZHTTP_REQUEST_BODY_MIDDLEWARE_H_
