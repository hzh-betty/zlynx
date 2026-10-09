#ifndef ZHTTP_HTTP_RESPONSE_H_
#define ZHTTP_HTTP_RESPONSE_H_

#include "zhttp/http_body.h"
#include "zhttp/http_common.h"
#include "zhttp/http_headers.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace znet { class Connection; }
namespace zhttp {
class HttpContext;
enum class WriteResult;
namespace HttpResponseWriter {
WriteResult send(const std::shared_ptr<znet::Connection> &, HttpContext &, bool);
}
// 状态行保留整数状态码，容纳常用枚举以外的合法 HTTP 状态。
/** 响应状态行；整数状态码允许表达常用枚举之外的 HTTP 状态。 */
struct HttpStatusLine {
    HttpVersion version = HttpVersion::HTTP_1_1;
    std::uint16_t status_code = 200;
};

/**
 * 可链式构造的 HTTP 响应。
 *
 * 写出前可修改；commit() 后除内部重置外，修改操作抛出 std::logic_error。
 * 正文由 HttpBody 独占持有，响应引用仅在所属 HttpContext 生命周期内有效。
 */
class HttpResponse {
  public:
    using ptr = std::shared_ptr<HttpResponse>;
    using Headers = HttpHeaders;
    using StreamCallback = HttpBody::StreamCallback;
    /** 构造 HTTP/1.1、200、保持连接的空响应。 */
    HttpResponse() = default;
    /** 设置枚举状态码，返回当前响应引用。 */
    HttpResponse &status(HttpStatus status) {
        return this->status(static_cast<int>(status));
    }
    /**
     * 设置整数 HTTP 状态码。
     *
     * @param code 状态码，范围 100–599。
     * @return 当前响应引用。
     * @throws std::invalid_argument 状态码超出范围。
     */
    HttpResponse &status(int code);
    /** 返回状态码的枚举表示；可能不对应具名枚举项。 */
    HttpStatus status_code() const {
        return static_cast<HttpStatus>(line_.status_code);
    }
    /** 返回包含原始整数状态码的只读状态行。 */
    const HttpStatusLine &status_line() const { return line_; }
    /** 设置响应 HTTP 版本，写出时仅支持 HTTP/1.0 和 HTTP/1.1。 */
    void set_version(HttpVersion version) {
        check_mutable();
        line_.version = version;
    }
    /** 返回响应 HTTP 版本。 */
    HttpVersion version() const { return line_.version; }
    /** 替换全部同名字段，返回当前响应；非法字段抛出 std::invalid_argument。 */
    HttpResponse &header(const std::string &key, const std::string &value);
    /** 追加字段并保留重复项，返回当前响应。 */
    HttpResponse &append_header(const std::string &key,
                                const std::string &value) {
        check_mutable();
        headers_.append(key, value);
        return *this;
    }
    /** 合并 Vary 字段中的 token，忽略大小写去重，返回当前响应。 */
    HttpResponse &append_vary(const std::string &value);
    /** 返回只读响应头集合。 */
    const Headers &headers() const { return headers_; }
    /** 返回独立保存的 Set-Cookie 值列表。 */
    const std::vector<std::string> &set_cookies() const { return set_cookies_; }
    /** 设置 Content-Type，返回当前响应。 */
    HttpResponse &content_type(const std::string &type) {
        return header("Content-Type", type);
    }
    /** 接管内存正文，返回当前响应。 */
    HttpResponse &body(std::string bytes) {
        return body(HttpBody::memory(std::move(bytes)));
    }
    /** 接管正文数据源，返回当前响应。 */
    HttpResponse &body(HttpBody body) {
        check_mutable();
        body_ = std::move(body);
        return *this;
    }
    /** 返回内存正文；不会读取文件或拉取流。 */
    const std::string &body_content() const { return body_.content(); }
    /** 返回只读正文数据源。 */
    const HttpBody &body_source() const { return body_; }
    /** 设置 JSON 内容类型及正文原文，不执行 JSON 序列化，返回当前响应。 */
    HttpResponse &json(const std::string &bytes) {
        content_type("application/json; charset=utf-8");
        return body(bytes);
    }
    /** 设置 UTF-8 HTML 内容类型及正文，返回当前响应。 */
    HttpResponse &html(const std::string &bytes) {
        content_type("text/html; charset=utf-8");
        return body(bytes);
    }
    /** 设置 UTF-8 纯文本内容类型及正文，返回当前响应。 */
    HttpResponse &text(const std::string &bytes) {
        content_type("text/plain; charset=utf-8");
        return body(bytes);
    }
    /**
     * 构造普通重定向响应并清空正文。
     *
     * @param url Location 字段值。
     * @param code 状态码，默认 302。
     * @return 当前响应引用。
     */
    HttpResponse &redirect(const std::string &url,
                           HttpStatus code = HttpStatus::FOUND) {
        status(code);
        header("Location", url);
        return body("");
    }
    /** 返回响应保持连接标志；写出策略仍可能要求关闭连接。 */
    bool is_keep_alive() const { return keep_alive_; }
    /** 设置响应保持连接标志。 */
    void set_keep_alive(bool keep) {
        check_mutable();
        keep_alive_ = keep;
    }
    /** 设置 chunked 标志；关闭时清除已有流正文，返回当前响应。 */
    HttpResponse &enable_chunked(bool enabled = true) {
        check_mutable();
        chunked_ = enabled;
        if (!enabled && body_.kind() == HttpBody::Kind::Stream)
            body_ = HttpBody();
        return *this;
    }
    /** 返回显式 chunked 标志。 */
    bool is_chunked_enabled() const { return chunked_; }
    // 同步拉取数据，回调不能保存写出句柄或把响应交给其他线程处理。
    /**
     * 设置同步流正文。
     *
     * @param callback 当前连接处理内调用的拉取回调，返回 0 表示结束。
     * @param length 预期总字节数，默认未知；不一致时写出失败并关闭连接。
     * @return 当前响应引用。
     * @throws std::invalid_argument 回调为空。
     */
    HttpResponse &stream(StreamCallback callback,
                         std::uint64_t length = HttpBody::UnknownLength);
    /** 返回正文是否为同步流。 */
    bool has_stream_callback() const {
        return body_.kind() == HttpBody::Kind::Stream;
    }
    /** 返回响应是否已进入写出阶段。 */
    bool committed() const { return committed_; }
    /** Cookie 属性；默认 Path=/、HttpOnly、SameSite=Lax，不设置 Max-Age。 */
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
     * 追加一个 Set-Cookie 头
     *
     * @param name Cookie 名称。
     * @param value Cookie 值。
     * @param opt Cookie 属性；max_age 单位为秒，负数表示省略。
     * @return 当前响应引用。
     */
    HttpResponse &set_cookie(const std::string &name, const std::string &value,
                             const CookieOptions &opt = CookieOptions());

    /**
     * 通过 Max-Age=0 删除 Cookie
     *
     * @param name Cookie 名称。
     * @param opt 属性应与创建时的 Path 等保持一致。
     * @return 当前响应引用。
     */
    HttpResponse &delete_cookie(const std::string &name,
                                const CookieOptions &opt = CookieOptions());

  private:
    friend WriteResult HttpResponseWriter::send(const std::shared_ptr<znet::Connection> &,
                                                HttpContext &, bool);
    void commit() { committed_ = true; }
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

#endif // ZHTTP_HTTP_RESPONSE_H_
