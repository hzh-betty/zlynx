#ifndef ZHTTP_HTTP_REQUEST_H_
#define ZHTTP_HTTP_REQUEST_H_

#include "http_common.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace zhttp {

class Session;
class MultipartFormData;

/**
 * @brief HTTP 请求对象
 */
class HttpRequest {
public:
  using ptr = std::shared_ptr<HttpRequest>;
  using Headers = std::unordered_map<std::string, std::string>;
  using Params = std::unordered_map<std::string, std::string>;

  HttpRequest() = default;

  /**
   * @brief 获取 HTTP 方法
   */
  HttpMethod method() const { return method_; }

  /**
   * @brief 获取请求路径
   */
  const std::string &path() const { return path_; }

  /**
   * @brief 获取查询字符串
   */
  const std::string &query() const { return query_; }

  /**
   * @brief 获取 HTTP 版本
   */
  HttpVersion version() const { return version_; }

  /**
   * @brief 获取所有请求头
   */
  const Headers &headers() const { return headers_; }

  /**
   * @brief 获取请求体
   */
  const std::string &body() const { return body_; }

  /**
   * @brief 获取远端地址（例如 "127.0.0.1:12345"）
   * @note 由服务器在接收连接/请求时填充
   */
  const std::string &remote_addr() const { return remote_addr_; }

  /**
   * @brief 获取所有路径参数
   */
  const Params &path_params() const { return path_params_; }

  /**
   * @brief 获取所有查询参数
   */
  const Params &query_params() const { return query_params_; }

  /**
   * @brief 获取特定请求头
   * @param key 请求头名称（不区分大小写）
   * @param default_val 默认值
   * @return 请求头值
   */
  std::string header(const std::string &key,
                     const std::string &default_val = "") const;

  /**
   * @brief 获取路径参数（如 /user/:id 中的 id）
   * @param key 参数名称
   * @param default_val 默认值
   * @return 参数值
   */
  std::string path_param(const std::string &key,
                         const std::string &default_val = "") const;

  /**
   * @brief 获取查询参数
   * @param key 参数名称
   * @param default_val 默认值
   * @return 参数值
   */
  std::string query_param(const std::string &key,
                          const std::string &default_val = "") const;

  // ===================== Cookie =====================

  /**
   * @brief 获取 Cookie
   */
  std::string cookie(const std::string &key,
                     const std::string &default_val = "") const;

  /**
   * @brief 获取所有 Cookie（惰性解析）
   */
  const Params &cookies() const;

  // ===================== Session =====================

  /**
   * @brief 获取会话对象（通常由 SessionMiddleware 注入）
   */
  std::shared_ptr<Session> session() const { return session_; }

  /**
   * @brief 设置会话对象（中间件使用）
   */
  void set_session(std::shared_ptr<Session> session) {
    session_ = std::move(session);
  }

  // ===================== Multipart (File Upload) =====================

  /**
   * @brief 是否为 multipart/form-data
   */
  bool is_multipart() const;

  /**
   * @brief 解析 multipart/form-data（惰性解析，重复调用无副作用）
   */
  bool parse_multipart();

  /**
   * @brief 获取解析后的 multipart 数据；未解析/失败返回 nullptr
   */
  const MultipartFormData *multipart() const;

  /**
   * @brief 获取 multipart 解析错误（若有）
   */
  const std::string &multipart_error() const { return multipart_error_; }

  // ===================== Setters =====================

  /**
   * @brief 设置 HTTP 方法
   */
  void set_method(HttpMethod method) { method_ = method; }

  /**
   * @brief 设置请求路径
   */
  void set_path(const std::string &path) { path_ = path; }

  /**
   * @brief 设置查询字符串
   */
  void set_query(const std::string &query) { query_ = query; }

  /**
   * @brief 设置 HTTP 版本
   */
  void set_version(HttpVersion version) { version_ = version; }

  /**
   * @brief 设置请求头
   * @param key 请求头名称
   * @param value 请求头值
   */
  void set_header(const std::string &key, const std::string &value);

  /**
   * @brief 设置请求体
   */
  void set_body(const std::string &body) { body_ = body; }

  /**
   * @brief 设置请求体（移动语义）
   */
  void set_body(std::string &&body) { body_ = std::move(body); }

  /**
   * @brief 设置远端地址（服务器使用）
   */
  void set_remote_addr(const std::string &addr) { remote_addr_ = addr; }

  /**
   * @brief 设置远端地址（移动语义）
   */
  void set_remote_addr(std::string &&addr) { remote_addr_ = std::move(addr); }

  /**
   * @brief 设置路径参数
   * @param key 参数名称
   * @param value 参数值
   */
  void set_path_param(const std::string &key, const std::string &value);

  /**
   * @brief 解析查询字符串为参数
   */
  void parse_query_params();

  // ===================== 便捷方法 =====================

  /**
   * @brief 是否为 Keep-Alive 连接
   */
  bool is_keep_alive() const;

  /**
   * @brief 获取 Content-Length
   * @return 内容长度，未指定返回 0
   */
  size_t content_length() const;

  /**
   * @brief 获取 Content-Type
   */
  std::string content_type() const;

private:
  void parse_cookies_if_needed() const;

  HttpMethod method_ = HttpMethod::UNKNOWN;
  std::string path_;
  std::string query_;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  Headers headers_;
  std::string body_;
  std::string remote_addr_;
  Params path_params_;  // 路径参数
  Params query_params_; // 查询参数

  // Cookie
  mutable bool cookies_parsed_ = false;
  mutable Params cookies_;

  // Session
  std::shared_ptr<Session> session_;

  // Multipart
  mutable bool multipart_parsed_ = false;
  mutable std::shared_ptr<MultipartFormData> multipart_;
  mutable std::string multipart_error_;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_REQUEST_H_