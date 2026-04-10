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

bool ascii_iequals(const std::string &lhs, const char *rhs) {
  if (!rhs || lhs.size() != std::char_traits<char>::length(rhs)) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

void append_header_line(std::string *out,
                        const std::string &key,
                        const std::string &value) {
  out->append(key);
  out->append(": ");
  out->append(value);
  out->append("\r\n");
}

size_t estimate_serialized_size(const HttpResponse &response,
                                bool include_body) {
  size_t total = 64;
  if (include_body) {
    total += response.body_content().size();
  }

  for (const auto &pair : response.headers()) {
    total += pair.first.size() + pair.second.size() + 4;
  }
  for (const auto &cookie : response.set_cookies()) {
    total += cookie.size() + 14;
  }

  return total;
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

HttpResponse &HttpResponse::upgrade_to_websocket(
    WebSocketCallbacks callbacks,
    const WebSocketOptions &options) {
  websocket_upgrade_enabled_ = true;
  websocket_callbacks_ = std::move(callbacks);
  websocket_options_ = options;

  // 协议升级与 HTTP chunked/stream 属于互斥路径，声明升级时清空流式状态。
  chunked_enabled_ = false;
  stream_callback_ = StreamCallback();
  async_stream_callback_ = AsyncStreamCallback();

  body_.clear();
  status_ = HttpStatus::SWITCHING_PROTOCOLS;
  keep_alive_ = true;
  return *this;
}

std::string HttpResponse::serialize(bool include_body) const {
  std::string out;
  serialize_to(&out, include_body);
  return out;
}

void HttpResponse::serialize_to(std::string *out, bool include_body) const {
  if (!out) {
    return;
  }

  const bool allow_body = is_body_allowed(status_);
  const bool use_chunked =
      allow_body && version_ == HttpVersion::HTTP_1_1 &&
      (chunked_enabled_ || static_cast<bool>(stream_callback_) ||
       static_cast<bool>(async_stream_callback_));
  bool has_content_length = false;
  bool has_connection = false;
  bool has_transfer_encoding = false;

  out->clear();
  out->reserve(estimate_serialized_size(*this, include_body));

  out->append(version_to_string(version_));
  out->push_back(' ');
  out->append(std::to_string(static_cast<int>(status_)));
  out->push_back(' ');
  out->append(status_to_string(status_));
  out->append("\r\n");

  // 普通响应头逐项输出。
  for (const auto &pair : headers_) {
    if (ascii_iequals(pair.first, "connection")) {
      has_connection = true;
    }

    if (ascii_iequals(pair.first, "content-length")) {
      if (use_chunked || !allow_body) {
        continue;
      }
      has_content_length = true;
    }

    if (ascii_iequals(pair.first, "transfer-encoding")) {
      if (!use_chunked || !allow_body) {
        continue;
      }
      has_transfer_encoding = true;
    }

    append_header_line(out, pair.first, pair.second);
  }

  // Set-Cookie 允许重复多次，所以单独输出每一条。
  for (const auto &val : set_cookies_) {
    out->append("Set-Cookie: ");
    out->append(val);
    out->append("\r\n");
  }

  if (use_chunked && !has_transfer_encoding) {
    out->append("Transfer-Encoding: chunked\r\n");
  }

  // 如果业务层没手动设置 Content-Length，这里自动补齐，避免客户端无法判断 Body 边界。
  if (allow_body && !use_chunked && !has_content_length) {
    out->append("Content-Length: ");
    out->append(std::to_string(body_.size()));
    out->append("\r\n");
  }

  // Connection 头用于告诉客户端当前连接是否还会继续复用。
  if (!has_connection) {
    out->append("Connection: ");
    out->append(keep_alive_ ? "keep-alive" : "close");
    out->append("\r\n");
  }

  // 头部结束后必须有一个空行，再接 Body。
  out->append("\r\n");

  // 最后直接拼接响应体原始内容。
  if (include_body && allow_body && !use_chunked) {
    out->append(body_);
  }
}

} // namespace zhttp
