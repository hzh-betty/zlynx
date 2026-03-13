#include "security_middleware.h"

namespace zhttp {

SecurityMiddleware::SecurityMiddleware(SecurityMiddleware::Options options)
    : options_(std::move(options)) {}

bool SecurityMiddleware::before(const HttpRequest::ptr &, HttpResponse &) {
  return true;
}

void SecurityMiddleware::after(const HttpRequest::ptr &,
                               HttpResponse &response) {
  // 仅在响应头缺失时才补齐，避免覆盖业务层显式设置的安全头。
  if (options_.set_x_frame_options && !options_.x_frame_options.empty()) {
    set_header_if_absent(response, "X-Frame-Options", options_.x_frame_options);
  }

  // 仅在响应头缺失时才补齐，避免覆盖业务层显式设置的安全头。
  if (options_.set_x_content_type_options &&
      !options_.x_content_type_options.empty()) {
    set_header_if_absent(response, "X-Content-Type-Options",
                         options_.x_content_type_options);
  }

  if (options_.set_referrer_policy && !options_.referrer_policy.empty()) {
    set_header_if_absent(response, "Referrer-Policy", options_.referrer_policy);
  }

  if (options_.set_content_security_policy &&
      !options_.content_security_policy.empty()) {
    set_header_if_absent(response, "Content-Security-Policy",
                         options_.content_security_policy);
  }

  if (options_.set_permissions_policy && !options_.permissions_policy.empty()) {
    set_header_if_absent(response, "Permissions-Policy",
                         options_.permissions_policy);
  }

  if (options_.set_hsts && !options_.hsts.empty()) {
    set_header_if_absent(response, "Strict-Transport-Security", options_.hsts);
  }
}

void SecurityMiddleware::set_header_if_absent(HttpResponse &response,
                                              const std::string &key,
                                              const std::string &value) const {
  if (response.headers().find(key) != response.headers().end()) {
    return;
  }

  response.header(key, value);
}

} // namespace zhttp