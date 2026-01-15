#ifndef ZHTTP_HTTP_REQUEST_H_
#define ZHTTP_HTTP_REQUEST_H_

#include "http_common.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief HTTP 请求对象
 */
class HttpRequest {
public:
  using ptr = std::shared_ptr<HttpRequest>;
  using Headers = std::unordered_map<std::string, std::string>;
  using Params = std::unordered_map<std::string, std::string>;

  HttpRequest() = default;

  // ===================== Getters =====================

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
  HttpMethod method_ = HttpMethod::UNKNOWN;
  std::string path_;
  std::string query_;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  Headers headers_;
  std::string body_;
  Params path_params_;  // 路径参数
  Params query_params_; // 查询参数
};

} // namespace zhttp

#endif // ZHTTP_HTTP_REQUEST_H_
