#pragma once
#include "zhttp/http_body.h"
#include "zhttp/http_common.h"
#include "zhttp/http_headers.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace zhttp {
// 状态行保留整数状态码，容纳常用枚举以外的合法 HTTP 状态。
struct HttpStatusLine {
    HttpVersion version = HttpVersion::HTTP_1_1;
    std::uint16_t status_code = 200;
};

class HttpResponse {
  public:
    using ptr = std::shared_ptr<HttpResponse>;
    using Headers = HttpHeaders;
    using StreamCallback = HttpBody::StreamCallback;
    HttpResponse() = default;
    HttpResponse &status(HttpStatus status) {
        return this->status(static_cast<int>(status));
    }
    HttpResponse &status(int code);
    HttpStatus status_code() const {
        return static_cast<HttpStatus>(line_.status_code);
    }
    const HttpStatusLine &status_line() const { return line_; }
    void set_version(HttpVersion version) {
        check_mutable();
        line_.version = version;
    }
    HttpVersion version() const { return line_.version; }
    HttpResponse &header(const std::string &key, const std::string &value);
    HttpResponse &append_header(const std::string &key,
                                const std::string &value) {
        check_mutable();
        headers_.append(key, value);
        return *this;
    }
    HttpResponse &append_vary(const std::string &value);
    const Headers &headers() const { return headers_; }
    const std::vector<std::string> &set_cookies() const { return set_cookies_; }
    HttpResponse &content_type(const std::string &type) {
        return header("Content-Type", type);
    }
    HttpResponse &body(std::string bytes) {
        return body(HttpBody::memory(std::move(bytes)));
    }
    HttpResponse &body(HttpBody body) {
        check_mutable();
        body_ = std::move(body);
        return *this;
    }
    const std::string &body_content() const { return body_.content(); }
    const HttpBody &body_source() const { return body_; }
    HttpResponse &json(const std::string &bytes) {
        content_type("application/json; charset=utf-8");
        return body(bytes);
    }
    HttpResponse &html(const std::string &bytes) {
        content_type("text/html; charset=utf-8");
        return body(bytes);
    }
    HttpResponse &text(const std::string &bytes) {
        content_type("text/plain; charset=utf-8");
        return body(bytes);
    }
    HttpResponse &redirect(const std::string &url,
                           HttpStatus code = HttpStatus::FOUND) {
        status(code);
        header("Location", url);
        return body("");
    }
    bool is_keep_alive() const { return keep_alive_; }
    void set_keep_alive(bool keep) {
        check_mutable();
        keep_alive_ = keep;
    }
    HttpResponse &enable_chunked(bool enabled = true) {
        check_mutable();
        chunked_ = enabled;
        if (!enabled && body_.kind() == HttpBody::Kind::Stream)
            body_ = HttpBody();
        return *this;
    }
    bool is_chunked_enabled() const { return chunked_; }
    // 同步拉取数据，回调不能保存写出句柄或把响应交给其他线程处理。
    HttpResponse &stream(StreamCallback callback,
                         std::uint64_t length = HttpBody::UnknownLength);
    bool has_stream_callback() const {
        return body_.kind() == HttpBody::Kind::Stream;
    }
    bool committed() const { return committed_; }
    void commit() { committed_ = true; }
    struct CookieOptions {
        CookieOptions()
            : path("/"), max_age(-1), http_only(true), secure(false),
              same_site("Lax") {}
        CookieOptions(std::string p, int maxAge, bool httpOnly, bool sec,
                      std::string sameSite)
            : path(std::move(p)), max_age(maxAge), http_only(httpOnly),
              secure(sec), same_site(std::move(sameSite)) {}

        std::string path;
        int max_age; // 秒；<0 表示不设置 Max-Age
        bool http_only;
        bool secure;
        std::string same_site; // Lax/Strict/None
    };

    /**
     * @brief 追加一个 Set-Cookie 头
     */
    HttpResponse &set_cookie(const std::string &name, const std::string &value,
                             const CookieOptions &opt = CookieOptions());

    /**
     * @brief 通过 Max-Age=0 删除 Cookie
     */
    HttpResponse &delete_cookie(const std::string &name,
                                const CookieOptions &opt = CookieOptions());

  private:
    void check_mutable() const;
    static std::string build_set_cookie_value(const std::string &name,
                                              const std::string &value,
                                              const CookieOptions &opt);
    HttpStatusLine line_;
    Headers headers_;
    std::vector<std::string> set_cookies_;
    HttpBody body_;
    bool keep_alive_ = true, chunked_ = false, committed_ = false;
};
} // namespace zhttp
