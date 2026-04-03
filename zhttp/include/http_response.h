#ifndef ZHTTP_HTTP_RESPONSE_H_
#define ZHTTP_HTTP_RESPONSE_H_

#include "http_common.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zhttp {

/**
 * @brief HTTP 响应对象
 * @details
 * 业务处理器通常通过链式调用构造响应，例如先设置状态码，再补头部，最后写入
 * Body。所有字段最终会由 serialize() 组装成标准 HTTP 响应报文。
 */
class HttpResponse {
public:
  using ptr = std::shared_ptr<HttpResponse>;
  using Headers = std::unordered_map<std::string, std::string>;
  // 返回写入到 buffer 的字节数；返回 0 表示流结束。
  using StreamCallback = std::function<size_t(char *, size_t)>;
  // 推送一个业务 chunk；返回 false 表示连接不可再写。
  using AsyncChunkSender = std::function<bool(const std::string &)>;
  // 结束异步流，触发终止块发送与连接收尾。
  using AsyncStreamCloser = std::function<void()>;
  // 业务层拿到 sender/closer 后可在任意时机异步推送 chunk。
  using AsyncStreamCallback =
      std::function<void(AsyncChunkSender, AsyncStreamCloser)>;

  HttpResponse();

  /**
   * @brief 设置状态码
   * @param status HTTP 状态码
   * @return 当前对象引用（支持链式调用）
   */
  HttpResponse &status(HttpStatus status);

  /**
   * @brief 设置状态码（整数形式）
   * @param code 状态码数值
   * @return 当前对象引用
   */
  HttpResponse &status(int code);

  /**
   * @brief 设置响应头
   * @param key 头部名称
   * @param value 头部值
   * @return 当前对象引用
   */
  HttpResponse &header(const std::string &key, const std::string &value);

  /**
   * @brief 设置 Content-Type
   * @param type MIME 类型
   * @return 当前对象引用
   */
  HttpResponse &content_type(const std::string &type);

  /**
   * @brief 设置响应体
   * @param body 响应内容
   * @return 当前对象引用
   */
  HttpResponse &body(const std::string &body);

  /**
   * @brief 设置响应体（移动语义）
   * @param body 响应内容
   * @return 当前对象引用
   */
  HttpResponse &body(std::string &&body);

  /**
   * @brief 返回 JSON 响应
   * @param json_str JSON 字符串
   * @return 当前对象引用
   */
  HttpResponse &json(const std::string &json_str);

  /**
   * @brief 返回 HTML 响应
   * @param html_str HTML 字符串
   * @return 当前对象引用
   */
  HttpResponse &html(const std::string &html_str);

  /**
   * @brief 返回纯文本响应
   * @param text_str 文本内容
   * @return 当前对象引用
   */
  HttpResponse &text(const std::string &text_str);

  /**
   * @brief 重定向
   * @param url 目标 URL
   * @param redirect_status 重定向状态码（默认 302）
   * @return 当前对象引用
   */
  HttpResponse &redirect(const std::string &url,
                         HttpStatus redirect_status = HttpStatus::FOUND);

  /**
   * @brief 获取状态码
    * @return 当前响应状态码
   */
  HttpStatus status_code() const { return status_; }

  /**
   * @brief 获取所有响应头
    * @return 普通响应头映射表，不包含 Set-Cookie 列表
   */
  const Headers &headers() const { return headers_; }

  /**
   * @brief 获取所有 Set-Cookie 值（每个元素对应一个 Set-Cookie 头部的 value 部分）
   */
  const std::vector<std::string> &set_cookies() const { return set_cookies_; }

  /**
   * @brief 获取响应体
    * @return 当前响应体内容
   */
  const std::string &body_content() const { return body_; }

  /**
   * @brief 是否 Keep-Alive
    * @return 当前响应是否期望保持连接
   */
  bool is_keep_alive() const { return keep_alive_; }

  /**
   * @brief 设置 Keep-Alive
    * @param keep_alive 是否保持连接
   */
  void set_keep_alive(bool keep_alive) { keep_alive_ = keep_alive; }

  /**
   * @brief 设置 HTTP 版本
    * @param version 要用于序列化响应行的版本
   */
  void set_version(HttpVersion version) { version_ = version; }

  /**
   * @brief 获取 HTTP 版本
   */
  HttpVersion version() const { return version_; }

  /**
   * @brief 显式启用/禁用 chunked 响应
   * @note 仅在 HTTP/1.1 且允许响应体时生效
   */
  HttpResponse &enable_chunked(bool enable = true);

  /**
   * @brief 配置同步流式回调（拉取模式）
    * @details 服务器会循环调用该回调获取下一段数据，返回 0 表示结束。
    * 每次返回的字节序列都会被编码为一个 chunk 帧。
   */
  HttpResponse &stream(StreamCallback callback);

  /**
   * @brief 配置异步流式回调（推送模式）
    * @details 业务层通过 sender 逐块发送，完成后必须调用 closer 结束响应。
    * sender 只负责发送数据块，终止块由 closer 统一触发。
   */
  HttpResponse &async_stream(AsyncStreamCallback callback);

  /**
   * @brief 当前响应是否启用了 chunked 发送策略
   */
  bool is_chunked_enabled() const { return chunked_enabled_; }

  /**
   * @brief 是否存在流式回调
   */
  bool has_stream_callback() const {
    return static_cast<bool>(stream_callback_);
  }

  /**
   * @brief 获取流式回调
   */
  const StreamCallback &stream_callback() const { return stream_callback_; }

  /**
   * @brief 是否存在异步流式回调
   */
  bool has_async_stream_callback() const {
    return static_cast<bool>(async_stream_callback_);
  }

  /**
   * @brief 获取异步流式回调
   */
  const AsyncStreamCallback &async_stream_callback() const {
    return async_stream_callback_;
  }

  /**
   * @brief 序列化为 HTTP 响应字符串
    * @param include_body 是否拼接响应体
    * @details chunked 路径常用 include_body=false 仅发响应头，body 走后续分块发送。
   * @return 完整的 HTTP 响应字符串
   */
  std::string serialize(bool include_body = true) const;


  /**
   * @brief Cookie 附加选项
   * @details
   * 这些选项会被拼接到 Set-Cookie 响应头中，用于控制 Cookie 的作用域和安全属性。
   */
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
  /**
   * @brief 构造单条 Set-Cookie 头部的 value 部分
   * @param name Cookie 名称
   * @param value Cookie 值
   * @param opt Cookie 附加属性
   * @return 组装完成的 Set-Cookie value
   */
  static std::string build_set_cookie_value(const std::string &name,
                                            const std::string &value,
                                            const CookieOptions &opt);

  // 响应的基础元数据和内容。
  HttpStatus status_ = HttpStatus::OK;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  Headers headers_;

  // Set-Cookie 允许重复出现，因此单独保存，序列化时逐条输出。
  std::vector<std::string> set_cookies_;
  std::string body_;
  bool keep_alive_ = true;
  bool chunked_enabled_ = false;
  StreamCallback stream_callback_;
  AsyncStreamCallback async_stream_callback_;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_RESPONSE_H_
