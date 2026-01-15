#ifndef ZHTTP_HTTP_COMMON_H_
#define ZHTTP_HTTP_COMMON_H_

#include <string>

namespace zhttp {

/**
 * @brief HTTP 方法枚举
 */
enum class HttpMethod {
  GET,
  POST,
  PUT,
  DELETE,
  HEAD,
  OPTIONS,
  PATCH,
  CONNECT,
  TRACE,
  UNKNOWN
};

/**
 * @brief HTTP 状态码
 */
enum class HttpStatus {
  // 1xx Informational
  CONTINUE = 100,
  SWITCHING_PROTOCOLS = 101,

  // 2xx Success
  OK = 200,
  CREATED = 201,
  ACCEPTED = 202,
  NO_CONTENT = 204,

  // 3xx Redirection
  MOVED_PERMANENTLY = 301,
  FOUND = 302,
  SEE_OTHER = 303,
  NOT_MODIFIED = 304,
  TEMPORARY_REDIRECT = 307,
  PERMANENT_REDIRECT = 308,

  // 4xx Client Error
  BAD_REQUEST = 400,
  UNAUTHORIZED = 401,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  METHOD_NOT_ALLOWED = 405,
  REQUEST_TIMEOUT = 408,
  CONFLICT = 409,
  LENGTH_REQUIRED = 411,
  PAYLOAD_TOO_LARGE = 413,
  URI_TOO_LONG = 414,
  UNSUPPORTED_MEDIA_TYPE = 415,
  TOO_MANY_REQUESTS = 429,

  // 5xx Server Error
  INTERNAL_SERVER_ERROR = 500,
  NOT_IMPLEMENTED = 501,
  BAD_GATEWAY = 502,
  SERVICE_UNAVAILABLE = 503,
  GATEWAY_TIMEOUT = 504,
  HTTP_VERSION_NOT_SUPPORTED = 505,
};

/**
 * @brief HTTP 版本
 */
enum class HttpVersion { HTTP_1_0, HTTP_1_1, UNKNOWN };

/**
 * @brief 将 HttpMethod 转换为字符串
 * @param method HTTP 方法
 * @return 方法名称字符串
 */
const char *method_to_string(HttpMethod method);

/**
 * @brief 将字符串转换为 HttpMethod
 * @param str 方法名称字符串
 * @return HTTP 方法枚举值
 */
HttpMethod string_to_method(const std::string &str);

/**
 * @brief 将 HttpStatus 转换为描述字符串
 * @param status HTTP 状态码
 * @return 状态描述字符串
 */
const char *status_to_string(HttpStatus status);

/**
 * @brief 获取文件扩展名对应的 MIME 类型
 * @param extension 文件扩展名（不带点）
 * @return MIME 类型字符串
 */
const char *get_mime_type(const std::string &extension);

/**
 * @brief 将 HttpVersion 转换为字符串
 * @param version HTTP 版本
 * @return 版本字符串 (如 "HTTP/1.1")
 */
const char *version_to_string(HttpVersion version);

/**
 * @brief 将字符串转换为 HttpVersion
 * @param str 版本字符串
 * @return HTTP 版本枚举值
 */
HttpVersion string_to_version(const std::string &str);

} // namespace zhttp

#endif // ZHTTP_HTTP_COMMON_H_
