#include "http_response.h"

#include <sstream>

namespace zhttp {

HttpResponse::HttpResponse() {
  // 设置默认响应头
  headers_["Server"] = "zhttp/1.0";
}

std::string HttpResponse::build_set_cookie_value(const std::string &name,
                                                 const std::string &value,
                                                 const CookieOptions &opt) {
  std::ostringstream oss;
  oss << name << "=" << value;

  if (!opt.path.empty()) {
    oss << "; Path=" << opt.path;
  }

  if (opt.max_age >= 0) {
    oss << "; Max-Age=" << opt.max_age;
  }

  if (opt.http_only) {
    oss << "; HttpOnly";
  }
  if (opt.secure) {
    oss << "; Secure";
  }

  if (!opt.same_site.empty()) {
    oss << "; SameSite=" << opt.same_site;
  }

  return oss.str();
}

HttpResponse &HttpResponse::set_cookie(const std::string &name,
                                       const std::string &value,
                                       const CookieOptions &opt) {
  set_cookies_.push_back(build_set_cookie_value(name, value, opt));
  return *this;
}

HttpResponse &HttpResponse::delete_cookie(const std::string &name,
                                          const CookieOptions &opt) {
  CookieOptions o = opt;
  o.max_age = 0;
  set_cookies_.push_back(build_set_cookie_value(name, "", o));
  return *this;
}

HttpResponse &HttpResponse::status(HttpStatus status) {
  status_ = status;
  return *this;
}

HttpResponse &HttpResponse::status(int code) {
  status_ = static_cast<HttpStatus>(code);
  return *this;
}

HttpResponse &HttpResponse::header(const std::string &key,
                                   const std::string &value) {
  headers_[key] = value;
  return *this;
}

HttpResponse &HttpResponse::content_type(const std::string &type) {
  headers_["Content-Type"] = type;
  return *this;
}

HttpResponse &HttpResponse::body(const std::string &body) {
  body_ = body;
  return *this;
}

HttpResponse &HttpResponse::body(std::string &&body) {
  body_ = std::move(body);
  return *this;
}

HttpResponse &HttpResponse::json(const std::string &json_str) {
  content_type("application/json; charset=utf-8");
  body_ = json_str;
  return *this;
}

HttpResponse &HttpResponse::html(const std::string &html_str) {
  content_type("text/html; charset=utf-8");
  body_ = html_str;
  return *this;
}

HttpResponse &HttpResponse::text(const std::string &text_str) {
  content_type("text/plain; charset=utf-8");
  body_ = text_str;
  return *this;
}

HttpResponse &HttpResponse::redirect(const std::string &url,
                                     HttpStatus redirect_status) {
  status_ = redirect_status;
  headers_["Location"] = url;
  body_.clear();
  return *this;
}

std::string HttpResponse::serialize() const {
  std::ostringstream oss;

  // 状态行
  oss << version_to_string(version_) << " " << static_cast<int>(status_) << " "
      << status_to_string(status_) << "\r\n";

  // 响应头
  for (const auto &pair : headers_) {
    oss << pair.first << ": " << pair.second << "\r\n";
  }

  // Set-Cookie（允许多值）
  for (const auto &val : set_cookies_) {
    oss << "Set-Cookie: " << val << "\r\n";
  }

  // Content-Length（如果没有设置且有 body）
  if (headers_.find("Content-Length") == headers_.end()) {
    oss << "Content-Length: " << body_.size() << "\r\n";
  }

  // Connection
  if (headers_.find("Connection") == headers_.end()) {
    oss << "Connection: " << (keep_alive_ ? "keep-alive" : "close") << "\r\n";
  }

  // 空行
  oss << "\r\n";

  // 响应体
  oss << body_;

  return oss.str();
}

} // namespace zhttp
