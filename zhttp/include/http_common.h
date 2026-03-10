#ifndef ZHTTP_HTTP_COMMON_H_
#define ZHTTP_HTTP_COMMON_H_

#include <string>

namespace zhttp {

/**
 * @brief HTTP 请求方法枚举
 *
 * 这些值对应请求行里的 METHOD 部分，例如 GET /users HTTP/1.1 中的 GET。
 * 解析器会把原始字符串转成该枚举，路由器再基于该枚举做分发。
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
 * @brief HTTP 状态码枚举
 *
 * 这里只收录框架内常用的一部分状态码，足够覆盖大多数 Web 服务场景。
 * 响应对象内部统一使用该枚举，最终序列化时再转成数字和状态短语。
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
 * @brief HTTP 协议版本
 *
 * 当前主要支持 HTTP/1.0 和 HTTP/1.1。UNKNOWN 用于表示解析失败
 * 或者遇到了当前实现尚未支持的版本字符串。
 */
enum class HttpVersion { HTTP_1_0, HTTP_1_1, UNKNOWN };

/**
 * @brief 将 HttpMethod 转换为字符串
 * @param method HTTP 方法
 * @return 方法名称字符串，例如 "GET"
 */
const char *method_to_string(HttpMethod method);

/**
 * @brief 将字符串转换为 HttpMethod
 * @param str 方法名称字符串，例如 "POST"
 * @return HTTP 方法枚举值；无法识别时返回 HttpMethod::UNKNOWN
 */
HttpMethod string_to_method(const std::string &str);

/**
 * @brief 将 HttpStatus 转换为描述字符串
 * @param status HTTP 状态码
 * @return 状态描述字符串，例如 "OK"、"Not Found"
 */
const char *status_to_string(HttpStatus status);

/**
 * @brief 获取文件扩展名对应的 MIME 类型
 * @param extension 文件扩展名（不带点），例如 "html"、"png"
 * @return MIME 类型字符串；未知扩展名通常会回退到通用类型
 */
const char *get_mime_type(const std::string &extension);

/**
 * @brief 将 HttpVersion 转换为字符串
 * @param version HTTP 版本
 * @return 版本字符串，例如 "HTTP/1.1"
 */
const char *version_to_string(HttpVersion version);

/**
 * @brief 将字符串转换为 HttpVersion
 * @param str 版本字符串，例如 "HTTP/1.1"
 * @return HTTP 版本枚举值；无法识别时返回 HttpVersion::UNKNOWN
 */
HttpVersion string_to_version(const std::string &str);

} // namespace zhttp

#endif // ZHTTP_HTTP_COMMON_H_
