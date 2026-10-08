/**
 * http_common.h
 * HTTP 通用类型、字符串、MIME、编码协商与日期工具。
 */
#ifndef ZHTTP_HTTP_COMMON_H_
#define ZHTTP_HTTP_COMMON_H_

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace zhttp {
/** 框架支持的 HTTP 请求方法，UNKNOWN 表示未识别方法。 */
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
/** 返回方法的大写名称；未识别值返回 UNKNOWN。 */
const char *method_to_string(HttpMethod method);
/** 忽略大小写解析方法，未识别时返回 UNKNOWN。 */
HttpMethod string_to_method(const std::string &str);

/** HTTP/1.x 版本与未识别标记。 */
enum class HttpVersion { HTTP_1_0, HTTP_1_1, UNKNOWN };
/** 返回 HTTP 版本文本，未识别值回退为 HTTP/1.1。 */
const char *version_to_string(HttpVersion version);
/** 解析 HTTP 版本文本，不支持时返回 UNKNOWN。 */
HttpVersion string_to_version(const std::string &str);

/** 常用 HTTP 响应状态码。 */
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
/** 按状态码百位划分的响应类别。 */
enum class StatusCategory {
    Informational = 1,
    Success,
    Redirection,
    ClientError,
    ServerError
};
/** 依据整数状态码的百位返回类别，不校验范围。 */
inline StatusCategory status_category(std::uint16_t code) {
    return static_cast<StatusCategory>(code / 100);
}
/** 返回状态码对应的原因短语，未识别时返回空串。 */
const char *status_to_string(HttpStatus status);
/** 判断状态码是否允许正文，排除 1xx、204、205 和 304。 */
bool is_body_allowed(HttpStatus status);

/** 依据文件扩展名查找 MIME 类型，未知扩展名回退为 application/octet-stream。 */
const char *get_mime_type(const std::string &extension);
/** 返回转为小写的字符串副本。 */
std::string to_lower(const std::string &str);
/** 原地移除字符串首尾空白。 */
void trim(std::string &str);
/** 按逗号分隔并忽略大小写查询完整 token，忽略两端空白。 */
bool header_contains_token(const std::string &header_value,
                           const std::string &token);
/** 移除 MIME 参数与首尾空白，并转为小写。 */
std::string normalize_mime_type(const std::string &content_type);
/** 按单字符分隔字符串并返回各片段。 */
std::vector<std::string> split_string(const std::string &str, char delimiter);
/** 用指定分隔串连接所有元素。 */
std::string join_string(const std::vector<std::string> &values,
                        const std::string &delimiter);
// Percent decoding, with '+' interpreted as a space for query/form values.
/** 解码有效百分号序列，将 + 转为空格；无效百分号序列保持原样。 */
std::string url_decode(const std::string &str);

// 按客户端权重排列可用编码；同权重优先 br，再 gzip，空串表示 identity。
// 未显式声明的 identity 作为最后回退；空结果表示没有可接受的编码。
/**
 * 按客户端权重返回允许的内容编码。
 *
 * @param header Accept-Encoding 原文。
 * @param enable_br 是否允许 Brotli。
 * @param enable_gzip 是否允许 gzip。
 * @return 同权重优先 br 再 gzip；空串元素表示 identity，空列表表示无可接受编码。
 */
std::vector<std::string> accepted_content_encodings(const std::string &header,
                                                    bool enable_br,
                                                    bool enable_gzip);

// 将 Unix 时间戳格式化为 HTTP GMT 日期。
/** 将 Unix 时间戳转换为 HTTP GMT 日期字符串。 */
std::string format_http_date_gmt(std::time_t timestamp);
} // namespace zhttp
#endif
