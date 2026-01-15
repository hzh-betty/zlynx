#ifndef ZHTTP_HTTP_RESPONSE_H_
#define ZHTTP_HTTP_RESPONSE_H_

#include "http_common.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief HTTP 响应对象
 */
class HttpResponse {
public:
  using ptr = std::shared_ptr<HttpResponse>;
  using Headers = std::unordered_map<std::string, std::string>;

  HttpResponse();

  // ===================== 链式调用设置方法 =====================

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

  // ===================== 便捷方法 =====================

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

  // ===================== Getters =====================

  /**
   * @brief 获取状态码
   */
  HttpStatus status_code() const { return status_; }

  /**
   * @brief 获取所有响应头
   */
  const Headers &headers() const { return headers_; }

  /**
   * @brief 获取响应体
   */
  const std::string &body_content() const { return body_; }

  /**
   * @brief 是否 Keep-Alive
   */
  bool is_keep_alive() const { return keep_alive_; }

  // ===================== 其他方法 =====================

  /**
   * @brief 设置 Keep-Alive
   */
  void set_keep_alive(bool keep_alive) { keep_alive_ = keep_alive; }

  /**
   * @brief 设置 HTTP 版本
   */
  void set_version(HttpVersion version) { version_ = version; }

  /**
   * @brief 序列化为 HTTP 响应字符串
   * @return 完整的 HTTP 响应字符串
   */
  std::string serialize() const;

private:
  HttpStatus status_ = HttpStatus::OK;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  Headers headers_;
  std::string body_;
  bool keep_alive_ = true;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_RESPONSE_H_
