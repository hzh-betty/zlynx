#ifndef ZHTTP_SECURITY_MIDDLEWARE_H_
#define ZHTTP_SECURITY_MIDDLEWARE_H_

#include "zhttp/mid/middleware.h"

#include <string>

namespace zhttp {
namespace mid {
/**
 * @brief 安全响应头中间件
 * @details
 * 在响应阶段统一补充常见 Web 安全响应头，降低点击劫持、MIME 混淆、
 * 不安全引用来源等风险。默认策略尽量保守且通用。
 */
class SecurityMiddleware : public Middleware {
  public:
    struct Options {
        Options()
            : set_x_frame_options(true), x_frame_options("DENY"),
              set_x_content_type_options(true),
              x_content_type_options("nosniff"), set_referrer_policy(true),
              referrer_policy("strict-origin-when-cross-origin"),
              set_content_security_policy(true),
              content_security_policy(
                  "default-src 'self'; base-uri 'self'; object-src 'none'; "
                  "frame-ancestors 'none'"),
              set_permissions_policy(true),
              permissions_policy("geolocation=(), camera=(), microphone=()"),
              set_hsts(false), hsts("max-age=31536000; includeSubDomains") {}

        bool set_x_frame_options; // 是否设置 X-Frame-Options 以防止点击劫持
        std::string x_frame_options; // X-Frame-Options 的值，常见选项包括
                                     // DENY、SAMEORIGIN 和 ALLOW-FROM uri

        bool set_x_content_type_options; // 是否设置 X-Content-Type-Options
                                         // 以防止 MIME 混淆
        std::string x_content_type_options; // X-Content-Type-Options
                                            // 的值，通常为 nosniff

        bool set_referrer_policy; // 是否设置 Referrer-Policy
        std::string
            referrer_policy; // Referrer-Policy 的值，常见选项包括
                             // no-referrer、no-referrer-when-downgrade、origin、strict-origin-when-cross-origin
                             // 等

        bool set_content_security_policy; // 是否设置 Content-Security-Policy
                                          // 以防止 XSS 和数据注入攻击
        std::string content_security_policy;

        bool set_permissions_policy; // 是否设置 Permissions-Policy
        std::string permissions_policy;

        // 默认关闭，避免在纯 HTTP 环境下误配。
        bool set_hsts;
        std::string hsts;
    };

    explicit SecurityMiddleware(Options options = Options());

    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    // 为了避免覆盖业务层显式设置，只有目标头缺失时才补齐。
    void set_header_if_absent(HttpResponse &response, const std::string &key,
                              const std::string &value) const;

    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_SECURITY_MIDDLEWARE_H_
