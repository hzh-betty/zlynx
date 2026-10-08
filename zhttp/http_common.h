/**
 * @file http_common.h
 * @brief HTTP 通用类型、字符串、MIME、编码协商与日期工具。
 */
#ifndef ZHTTP_HTTP_COMMON_H_
#define ZHTTP_HTTP_COMMON_H_

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace zhttp {
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
const char *method_to_string(HttpMethod method);
HttpMethod string_to_method(const std::string &str);

enum class HttpVersion { HTTP_1_0, HTTP_1_1, UNKNOWN };
const char *version_to_string(HttpVersion version);
HttpVersion string_to_version(const std::string &str);

enum class HttpStatus {
    // 1xx Informational
    CONTINUE = 100, // 继续，表示客户端应继续发送请求的剩余部分；常见于 Expect:
                    // 100-continue 场景
    SWITCHING_PROTOCOLS = 101, // 切换协议，例如升级到 WebSocket

    // 2xx Success
    OK = 200,         // 请求成功，响应体包含请求的资源或处理结果
    CREATED = 201,    // 已创建，表示请求已成功处理并创建了新的资源
    ACCEPTED = 202,   // 已接受，表示请求已被接受但尚未处理完成
    NO_CONTENT = 204, // 无内容，表示响应体中不包含任何内容
    RESET_CONTENT = 205,
    PARTIAL_CONTENT =
        206, // 部分内容，表示响应体只包含请求范围内的一部分；常见于 Range 请求

    // 3xx Redirection
    MOVED_PERMANENTLY = 301, // 永久移动，表示请求的资源已被永久移动到新
                             // URL，响应中会包含 Location 头指向新地址
    FOUND = 302,     // 临时移动，表示请求的资源临时移动到了新 URL，响应中会包含
                     // Location 头指向新地址
    SEE_OTHER = 303, // See Other，表示请求的资源在另一个 URL，响应中会包含
                     // Location 头指向新地址
    NOT_MODIFIED = 304, // 未修改，表示客户端的缓存副本仍然有效，可以继续使用
    TEMPORARY_REDIRECT = 307, // 临时重定向，表示请求的资源临时移动到了新
                              // URL，响应中会包含 Location 头指向新地址
    PERMANENT_REDIRECT = 308, // 永久重定向，表示请求的资源永久移动到了新
                              // URL，响应中会包含 Location 头指向新地址

    // 4xx Client Error
    BAD_REQUEST = 400,        // 错误请求，表示请求语法错误或参数不合法
    UNAUTHORIZED = 401,       // 未授权，表示请求需要用户认证
    FORBIDDEN = 403,          // 禁止，表示服务器理解请求，但拒绝执行
    NOT_FOUND = 404,          // 未找到，表示请求的资源不存在
    METHOD_NOT_ALLOWED = 405, // 方法不允许，表示请求方法不被允许
    NOT_ACCEPTABLE = 406,
    REQUEST_TIMEOUT = 408,   // 请求超时，表示服务器等待请求时超时
    CONFLICT = 409,          // 冲突，表示请求与服务器当前状态冲突
    LENGTH_REQUIRED = 411,   // 长度要求，表示请求需要 Content-Length 头
    PAYLOAD_TOO_LARGE = 413, // 负载过大，表示请求体过大
    URI_TOO_LONG = 414,      // URI 过长，表示请求的 URI 过长
    UNSUPPORTED_MEDIA_TYPE =
        415, // 不支持的媒体类型，表示请求的 Content-Type 不被支持
    REQUESTED_RANGE_NOT_SATISFIABLE =
        416, // 请求范围不满足，表示请求的 Range 头无效
    EXPECTATION_FAILED = 417,
    TOO_MANY_REQUESTS = 429, // 请求过多，表示客户端发送了过多请求
    REQUEST_HEADER_FIELDS_TOO_LARGE = 431,

    // 5xx Server Error
    INTERNAL_SERVER_ERROR =
        500,               // 内部服务器错误，表示服务器在处理请求时发生了错误
    NOT_IMPLEMENTED = 501, // 未实现，表示服务器不支持当前请求的方法
    BAD_GATEWAY = 502,     // 错误网关，表示服务器作为网关或代理时收到了无效响应
    SERVICE_UNAVAILABLE = 503, // 服务不可用，表示服务器暂时无法处理请求
    GATEWAY_TIMEOUT = 504, // 网关超时，表示服务器作为网关或代理时等待响应超时
    HTTP_VERSION_NOT_SUPPORTED =
        505, // 不支持的 HTTP 版本，表示服务器不支持请求中使用的 HTTP 版本
};
enum class StatusCategory {
    Informational = 1,
    Success,
    Redirection,
    ClientError,
    ServerError
};
inline StatusCategory status_category(std::uint16_t code) {
    return static_cast<StatusCategory>(code / 100);
}
const char *status_to_string(HttpStatus status);
bool is_body_allowed(HttpStatus status);

const char *get_mime_type(const std::string &extension);
std::string to_lower(const std::string &str);
void trim(std::string &str);
bool header_contains_token(const std::string &header_value,
                           const std::string &token);
std::string normalize_mime_type(const std::string &content_type);
std::vector<std::string> split_string(const std::string &str, char delimiter);
std::string join_string(const std::vector<std::string> &values,
                        const std::string &delimiter);
// Percent decoding, with '+' interpreted as a space for query/form values.
std::string url_decode(const std::string &str);

// 按客户端权重排列可用编码；同权重优先 br，再 gzip，空串表示 identity。
// 未显式声明的 identity 作为最后回退；空结果表示没有可接受的编码。
std::vector<std::string> accepted_content_encodings(const std::string &header,
                                                    bool enable_br,
                                                    bool enable_gzip);

// 将 Unix 时间戳格式化为 HTTP GMT 日期。
std::string format_http_date_gmt(std::time_t timestamp);
} // namespace zhttp
#endif
