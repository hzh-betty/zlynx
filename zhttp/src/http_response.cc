#include "http_response.h"

#include <sstream>

namespace zhttp {

namespace {

bool is_body_allowed(HttpStatus status) {
  const int code = static_cast<int>(status);
  if (code >= 100 && code < 200) {
    return false;
  }
  return code != 204 && code != 304;
}

bool header_name_equals(const std::string &name, const std::string &target_lower) {
  return to_lower(name) == target_lower;
}

} // namespace

HttpResponse::HttpResponse() {
  // 提供一个默认 Server 头，业务层如果需要可以再覆盖。
  headers_["Server"] = "zhttp/1.0";
}

std::string HttpResponse::build_set_cookie_value(const std::string &name,
                                                 const std::string &value,
                                                 const CookieOptions &opt) {
  // 按 Set-Cookie 语法把主值和可选属性顺序拼接起来。
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
  // Set-Cookie 允许重复出现，所以不能简单塞进普通 headers_ 覆盖旧值。
  set_cookies_.push_back(build_set_cookie_value(name, value, opt));
  return *this;
}

HttpResponse &HttpResponse::delete_cookie(const std::string &name,
                                          const CookieOptions &opt) {
  // 通过 Max-Age=0 告诉浏览器立即删除该 Cookie。
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

HttpResponse &HttpResponse::enable_chunked(bool enable) {
  chunked_enabled_ = enable;
  if (!enable) {
    stream_callback_ = StreamCallback();
    async_stream_callback_ = AsyncStreamCallback();
  }
  return *this;
}

HttpResponse &HttpResponse::stream(StreamCallback callback) {
  stream_callback_ = std::move(callback);
  async_stream_callback_ = AsyncStreamCallback();
  chunked_enabled_ = true;
  return *this;
}

HttpResponse &HttpResponse::async_stream(AsyncStreamCallback callback) {
  async_stream_callback_ = std::move(callback);
  stream_callback_ = StreamCallback();
  chunked_enabled_ = true;
  // 异步推送阶段连接会在 close 回调里结束，避免同连接复用带来的乱序风险。
  keep_alive_ = false;
  return *this;
}

std::string HttpResponse::serialize(bool include_body) const {
  // HTTP 响应最终就是一段按协议格式拼好的纯文本字节流。
  std::ostringstream oss;
  const bool allow_body = is_body_allowed(status_);
  const bool use_chunked =
      allow_body && version_ == HttpVersion::HTTP_1_1 &&
      (chunked_enabled_ || static_cast<bool>(stream_callback_) ||
       static_cast<bool>(async_stream_callback_));
  bool has_content_length = false;
  bool has_connection = false;
  bool has_transfer_encoding = false;

  // 第一行是状态行：版本 + 数字状态码 + 状态短语。
  oss << version_to_string(version_) << " " << static_cast<int>(status_) << " "
      << status_to_string(status_) << "\r\n";

  // 普通响应头逐项输出。
  for (const auto &pair : headers_) {
    if (header_name_equals(pair.first, "connection")) {
      has_connection = true;
    }

    if (header_name_equals(pair.first, "content-length")) {
      if (use_chunked || !allow_body) {
        continue;
      }
      has_content_length = true;
    }

    if (header_name_equals(pair.first, "transfer-encoding")) {
      if (!use_chunked || !allow_body) {
        continue;
      }
      has_transfer_encoding = true;
    }

    oss << pair.first << ": " << pair.second << "\r\n";
  }

  // Set-Cookie 允许重复多次，所以单独输出每一条。
  for (const auto &val : set_cookies_) {
    oss << "Set-Cookie: " << val << "\r\n";
  }

  if (use_chunked && !has_transfer_encoding) {
    oss << "Transfer-Encoding: chunked\r\n";
  }

  // 如果业务层没手动设置 Content-Length，这里自动补齐，避免客户端无法判断 Body 边界。
  if (allow_body && !use_chunked && !has_content_length) {
    oss << "Content-Length: " << body_.size() << "\r\n";
  }

  // Connection 头用于告诉客户端当前连接是否还会继续复用。
  if (!has_connection) {
    oss << "Connection: " << (keep_alive_ ? "keep-alive" : "close") << "\r\n";
  }

  // 头部结束后必须有一个空行，再接 Body。
  oss << "\r\n";

  // 最后直接拼接响应体原始内容。
  if (include_body && allow_body && !use_chunked) {
    oss << body_;
  }

  return oss.str();
}

} // namespace zhttp
