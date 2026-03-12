#ifndef ZHTTP_HTTP_COMMON_H_
#define ZHTTP_HTTP_COMMON_H_

#include <ctime>
#include <string>
#include <vector>

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
  CONTINUE = 100, // 继续，表示客户端应继续发送请求的剩余部分；常见于 Expect: 100-continue 场景
  SWITCHING_PROTOCOLS = 101, // 切换协议，例如升级到 WebSocket

  // 2xx Success
  OK = 200, // 请求成功，响应体包含请求的资源或处理结果
  PARTIAL_CONTENT = 206, // 部分内容，表示响应体只包含请求范围内的一部分；常见于 Range 请求
  CREATED = 201, // 已创建，表示请求已成功处理并创建了新的资源
  ACCEPTED = 202, // 已接受，表示请求已被接受但尚未处理完成
  NO_CONTENT = 204, // 无内容，表示响应体中不包含任何内容

  // 3xx Redirection
  MOVED_PERMANENTLY = 301, // 永久移动，表示请求的资源已被永久移动到新 URL，响应中会包含 Location 头指向新地址
  FOUND = 302, // 临时移动，表示请求的资源临时移动到了新 URL，响应中会包含 Location 头指向新地址
  SEE_OTHER = 303, // See Other，表示请求的资源在另一个 URL，响应中会包含 Location 头指向新地址
  NOT_MODIFIED = 304, // 未修改，表示客户端的缓存副本仍然有效，可以继续使用
  TEMPORARY_REDIRECT = 307, // 临时重定向，表示请求的资源临时移动到了新 URL，响应中会包含 Location 头指向新地址
  PERMANENT_REDIRECT = 308, // 永久重定向，表示请求的资源永久移动到了新 URL，响应中会包含 Location 头指向新地址

  // 4xx Client Error
  BAD_REQUEST = 400, // 错误请求，表示请求语法错误或参数不合法
  UNAUTHORIZED = 401, // 未授权，表示请求需要用户认证
  FORBIDDEN = 403, // 禁止，表示服务器理解请求，但拒绝执行
  NOT_FOUND = 404, // 未找到，表示请求的资源不存在
  METHOD_NOT_ALLOWED = 405, // 方法不允许，表示请求方法不被允许
  REQUEST_TIMEOUT = 408, // 请求超时，表示服务器等待请求时超时
  CONFLICT = 409, // 冲突，表示请求与服务器当前状态冲突
  LENGTH_REQUIRED = 411, // 长度要求，表示请求需要 Content-Length 头
  PAYLOAD_TOO_LARGE = 413, // 负载过大，表示请求体过大
  URI_TOO_LONG = 414, // URI 过长，表示请求的 URI 过长
  UNSUPPORTED_MEDIA_TYPE = 415, // 不支持的媒体类型，表示请求的 Content-Type 不被支持
  REQUESTED_RANGE_NOT_SATISFIABLE = 416, // 请求范围不满足，表示请求的 Range 头无效
  TOO_MANY_REQUESTS = 429, // 请求过多，表示客户端发送了过多请求

  // 5xx Server Error
  INTERNAL_SERVER_ERROR = 500, // 内部服务器错误，表示服务器在处理请求时发生了错误
  NOT_IMPLEMENTED = 501, // 未实现，表示服务器不支持当前请求的方法
  BAD_GATEWAY = 502, // 错误网关，表示服务器作为网关或代理时收到了无效响应
  SERVICE_UNAVAILABLE = 503, // 服务不可用，表示服务器暂时无法处理请求
  GATEWAY_TIMEOUT = 504, // 网关超时，表示服务器作为网关或代理时等待响应超时
  HTTP_VERSION_NOT_SUPPORTED = 505, // 不支持的 HTTP 版本，表示服务器不支持请求中使用的 HTTP 版本
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

/**
 * @brief 将字符串拷贝为小写版本
 * @param str 输入字符串
 * @return 转换后的新字符串，不会修改入参
 * @details
 * 常用于 HTTP 头字段名、MIME 参数等大小写不敏感场景。
 */
std::string to_lower(const std::string &str);

/**
 * @brief 就地去除字符串首尾空白字符
 * @param str 待处理字符串（会被原地修改）
 * @details
 * 这里的空白字符判定遵循 `std::isspace`，包括空格、制表符、回车等。
 */
void trim(std::string &str);

/**
 * @brief 按指定分隔符切分字符串
 * @param str 输入字符串
 * @param delimiter 分隔符
 * @return 切分结果（保留空片段）
 * @details
 * 例如：split_string("a,,b", ',') -> {"a", "", "b"}
 */
std::vector<std::string> split_string(const std::string &str, char delimiter);

/**
 * @brief URL 解码（百分号编码）
 * @param str 输入字符串
 * @return 解码后的字符串
 * @details
 * - 支持 `%XX` 十六进制编码；
 * - 支持把 `+` 还原为空格（兼容 `application/x-www-form-urlencoded`）；
 * - 非法 `%` 序列保持原样，不抛异常。
 */
std::string url_decode(const std::string &str);

/**
 * @brief 将 Unix 时间戳格式化为 HTTP GMT 时间字符串
 * @param timestamp 秒级 Unix 时间戳
 * @return 形如 `Wed, 21 Oct 2015 07:28:00 GMT` 的字符串
 * @details
 * 主要用于 `Last-Modified`、`If-Modified-Since` 等 HTTP 缓存头。
 */
std::string format_http_date_gmt(std::time_t timestamp);

} // namespace zhttp

#endif // ZHTTP_HTTP_COMMON_H_
