/**
 * @file http_common.cc
 *
 * @author hzh-betty
 */
#include "zhttp/http_common.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace zhttp {
const char *method_to_string(HttpMethod method) {
    switch (method) {
    case HttpMethod::GET:
        return "GET";
    case HttpMethod::POST:
        return "POST";
    case HttpMethod::PUT:
        return "PUT";
    case HttpMethod::DELETE:
        return "DELETE";
    case HttpMethod::HEAD:
        return "HEAD";
    case HttpMethod::OPTIONS:
        return "OPTIONS";
    case HttpMethod::PATCH:
        return "PATCH";
    case HttpMethod::CONNECT:
        return "CONNECT";
    case HttpMethod::TRACE:
        return "TRACE";
    default:
        return "UNKNOWN";
    }
}

HttpMethod string_to_method(std::string_view str) {
    const auto matches = [str](std::string_view method) {
        return str.size() == method.size() &&
               std::equal(str.begin(), str.end(), method.begin(),
                          [](unsigned char c, unsigned char expected) {
                              return std::toupper(c) == expected;
                          });
    };
    if (matches("GET"))
        return HttpMethod::GET;
    if (matches("POST"))
        return HttpMethod::POST;
    if (matches("PUT"))
        return HttpMethod::PUT;
    if (matches("DELETE"))
        return HttpMethod::DELETE;
    if (matches("HEAD"))
        return HttpMethod::HEAD;
    if (matches("OPTIONS"))
        return HttpMethod::OPTIONS;
    if (matches("PATCH"))
        return HttpMethod::PATCH;
    if (matches("CONNECT"))
        return HttpMethod::CONNECT;
    if (matches("TRACE"))
        return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

const char *version_to_string(HttpVersion version) {
    switch (version) {
    case HttpVersion::HTTP_1_0:
        return "HTTP/1.0";
    case HttpVersion::HTTP_1_1:
        return "HTTP/1.1";
    default:
        return "HTTP/1.1";
    }
}

HttpVersion string_to_version(std::string_view str) {
    if (str == "HTTP/1.0") {
        return HttpVersion::HTTP_1_0;
    }
    if (str == "HTTP/1.1") {
        return HttpVersion::HTTP_1_1;
    }
    return HttpVersion::UNKNOWN;
}

const char *status_to_string(HttpStatus status) {
    switch (status) {
    // 1xx 信息响应
    case HttpStatus::CONTINUE:
        return "Continue";
    case HttpStatus::SWITCHING_PROTOCOLS:
        return "Switching Protocols";

    // 2xx 成功响应
    case HttpStatus::OK:
        return "OK";
    case HttpStatus::PARTIAL_CONTENT:
        return "Partial Content";
    case HttpStatus::CREATED:
        return "Created";
    case HttpStatus::ACCEPTED:
        return "Accepted";
    case HttpStatus::NO_CONTENT:
        return "No Content";
    case HttpStatus::RESET_CONTENT:
        return "Reset Content";
    case HttpStatus::EXPECTATION_FAILED:
        return "Expectation Failed";

    // 3xx 重定向响应
    case HttpStatus::MOVED_PERMANENTLY:
        return "Moved Permanently";
    case HttpStatus::FOUND:
        return "Found";
    case HttpStatus::SEE_OTHER:
        return "See Other";
    case HttpStatus::NOT_MODIFIED:
        return "Not Modified";
    case HttpStatus::TEMPORARY_REDIRECT:
        return "Temporary Redirect";
    case HttpStatus::PERMANENT_REDIRECT:
        return "Permanent Redirect";

    // 4xx 客户端错误
    case HttpStatus::BAD_REQUEST:
        return "Bad Request";
    case HttpStatus::UNAUTHORIZED:
        return "Unauthorized";
    case HttpStatus::FORBIDDEN:
        return "Forbidden";
    case HttpStatus::NOT_FOUND:
        return "Not Found";
    case HttpStatus::METHOD_NOT_ALLOWED:
        return "Method Not Allowed";
    case HttpStatus::NOT_ACCEPTABLE:
        return "Not Acceptable";
    case HttpStatus::REQUEST_TIMEOUT:
        return "Request Timeout";
    case HttpStatus::CONFLICT:
        return "Conflict";
    case HttpStatus::LENGTH_REQUIRED:
        return "Length Required";
    case HttpStatus::PAYLOAD_TOO_LARGE:
        return "Payload Too Large";
    case HttpStatus::URI_TOO_LONG:
        return "URI Too Long";
    case HttpStatus::UNSUPPORTED_MEDIA_TYPE:
        return "Unsupported Media Type";
    case HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE:
        return "Requested Range Not Satisfiable";
    case HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE:
        return "Request Header Fields Too Large";
    case HttpStatus::TOO_MANY_REQUESTS:
        return "Too Many Requests";

    // 5xx 服务器错误
    case HttpStatus::INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    case HttpStatus::NOT_IMPLEMENTED:
        return "Not Implemented";
    case HttpStatus::BAD_GATEWAY:
        return "Bad Gateway";
    case HttpStatus::SERVICE_UNAVAILABLE:
        return "Service Unavailable";
    case HttpStatus::GATEWAY_TIMEOUT:
        return "Gateway Timeout";
    case HttpStatus::HTTP_VERSION_NOT_SUPPORTED:
        return "HTTP Version Not Supported";
    default:
        return "";
    }
}

bool is_body_allowed(HttpStatus status) {
    const int code = static_cast<int>(status);
    return !(code >= 100 && code < 200) && code != 204 && code != 205 &&
           code != 304;
}

const char *get_mime_type(const std::string &extension) {
    static const std::unordered_map<std::string, const char *> mime_types = {
        // 文本
        {"html", "text/html"},
        {"htm", "text/html"},
        {"css", "text/css"},
        {"js", "application/javascript"},
        {"json", "application/json"},
        {"xml", "application/xml"},
        {"txt", "text/plain"},

        // 图像
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"webp", "image/webp"},

        // 音频／视频
        {"mp3", "audio/mpeg"},
        {"mp4", "video/mp4"},
        {"webm", "video/webm"},

        // 文档和归档
        {"pdf", "application/pdf"},
        {"zip", "application/zip"},
        {"gz", "application/gzip"},

        // 字体
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},

        // 其他类型
        {"wasm", "application/wasm"},
    };

    std::string lower = to_lower(extension);
    auto it = mime_types.find(lower);
    if (it != mime_types.end()) {
        return it->second;
    }
    return "application/octet-stream";
}

std::string to_lower(std::string_view str) {
    std::string lower(str);
    std::transform(
        lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

std::string_view trim_view(std::string_view str) {
    while (!str.empty() &&
           std::isspace(static_cast<unsigned char>(str.front())))
        str.remove_prefix(1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))
        str.remove_suffix(1);
    return str;
}

void trim(std::string &str) {
    const auto view = trim_view(str);
    const auto length = view.size();
    str.erase(0, static_cast<size_t>(view.data() - str.data()));
    str.resize(length);
}

std::string normalize_mime_type(std::string_view content_type) {
    return to_lower(trim_view(content_type.substr(0, content_type.find(';'))));
}

namespace {
bool equal_case_insensitive(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y) {
                          return std::tolower(x) == std::tolower(y);
                      });
}
} // namespace

bool header_contains_token(std::string_view header_value,
                           std::string_view target) {
    while (!header_value.empty()) {
        const auto comma = header_value.find(',');
        const auto token = trim_view(header_value.substr(0, comma));
        if (!token.empty() && equal_case_insensitive(token, target))
            return true;
        if (comma == std::string_view::npos)
            break;
        header_value.remove_prefix(comma + 1);
    }
    return false;
}

std::vector<std::string> split_string(std::string_view str, char delimiter) {
    std::vector<std::string> parts;

    size_t start = 0;
    while (start <= str.size()) {
        size_t pos = str.find(delimiter, start);
        if (pos == std::string::npos) {
            pos = str.size();
        }

        parts.emplace_back(str.substr(start, pos - start));

        if (pos == str.size()) {
            break;
        }
        start = pos + 1;
    }

    return parts;
}

std::string join_string(const std::vector<std::string> &values,
                        const std::string &delimiter) {
    if (values.empty()) {
        return {};
    }

    std::string output;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output += delimiter;
        }
        output += values[i];
    }
    return output;
}

std::string url_decode(std::string_view str) {
    // URL 解码整体策略：
    // 1) 顺序扫描输入字符串；
    // 2) 遇到合法 "%HH"（十六进制）则还原为单字节；
    // 3) 遇到 '+' 还原为空格（application/x-www-form-urlencoded 语义）；
    // 4) 其他字符原样保留。
    // 说明：非法或不完整的 '%' 序列不会报错，会按原字符输出。
    std::string output;

    // 解码后长度不会超过原串，先预留可减少扩容。
    output.reserve(str.size());

    // 将单个十六进制字符转为数值（0~15），非法返回 -1。
    auto hex_to_int = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };

    // 主循环：按字符推进；当成功消费 "%HH" 时会额外跳过 2 个字符。
    for (size_t i = 0; i < str.size(); ++i) {
        // 分支 A：尝试解析百分号编码（例如 "%2F" -> '/').
        if (str[i] == '%' && i + 2 < str.size()) {
            int hi = hex_to_int(str[i + 1]);
            int lo = hex_to_int(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                // 两位十六进制都合法：合并为一个字节写入输出。
                output.push_back(static_cast<char>(hi * 16 + lo));

                // 已消费当前位置和后两位，for 迭代再 +1
                // 后刚好指向下一个未处理字符。
                i += 2;
                continue;
            }

            // 非法 "%" 序列：降级为普通字符路径，保持输入可逆。
        } else if (str[i] == '+') {
            // 分支 B：'+' 按表单编码规则映射为空格。
            output.push_back(' ');
            continue;
        }

        // 分支 C：普通字符直接透传。
        output.push_back(str[i]);
    }

    return output;
}

namespace {

// qvalue 使用千分整数，避免浮点和宽松数值解析接受非法输入。
int parse_encoding_quality(std::string_view value) {
    if (value.empty() || (value[0] != '0' && value[0] != '1'))
        return 0;
    if (value.size() == 1)
        return value[0] == '1' ? 1000 : 0;
    if (value[1] != '.' || value.size() > 5)
        return 0;
    int quality = value[0] == '1' ? 1000 : 0;
    int place = 100;
    for (size_t i = 2; i < value.size(); ++i, place /= 10) {
        if (value[i] < '0' || value[i] > '9' ||
            (value[0] == '1' && value[i] != '0'))
            return 0;
        quality += (value[i] - '0') * place;
    }
    return quality;
}

} // namespace

std::vector<std::string> accepted_content_encodings(std::string_view header,
                                                    bool enable_br,
                                                    bool enable_gzip) {
    // 保留默认行为：未指定编码时返回原始实体。
    if (header.empty())
        return {""};
    int br = -1, gzip = -1, identity = -1, wildcard = -1;
    while (!header.empty()) {
        const auto comma = header.find(',');
        const auto item = header.substr(0, comma);
        header = comma == std::string_view::npos ? std::string_view{}
                                                 : header.substr(comma + 1);
        const auto semi = item.find(';');
        const auto token = trim_view(item.substr(0, semi));
        int *target = equal_case_insensitive(token, "br")         ? &br
                      : equal_case_insensitive(token, "gzip")     ? &gzip
                      : equal_case_insensitive(token, "identity") ? &identity
                      : token == "*"                              ? &wildcard
                                                                  : nullptr;
        if (!target)
            continue;
        int quality = 1000;
        if (semi != std::string_view::npos) {
            const auto parameter = item.substr(semi + 1);
            const auto equal = parameter.find('=');
            const auto name = trim_view(parameter.substr(0, equal));
            const auto value = equal == std::string_view::npos
                                   ? std::string_view{}
                                   : trim_view(parameter.substr(equal + 1));
            quality = equal_case_insensitive(name, "q")
                          ? parse_encoding_quality(value)
                          : 0;
        }

        // 重复项取较严格值，显式 q=0 不能被另一个重复项覆盖。
        *target = *target < 0 ? quality : std::min(*target, quality);
    }
    std::vector<std::pair<std::string, int>> ranked;
    const int br_quality = br < 0 ? wildcard : br;
    const int gzip_quality = gzip < 0 ? wildcard : gzip;
    if (enable_br && br_quality > 0)
        ranked.emplace_back("br", br_quality);
    if (enable_gzip && gzip_quality > 0)
        ranked.emplace_back("gzip", gzip_quality);
    if (identity > 0)
        ranked.emplace_back("", identity);
    std::stable_sort(
        ranked.begin(), ranked.end(),
        [](const auto &a, const auto &b) { return a.second > b.second; });
    std::vector<std::string> result;
    for (const auto &entry : ranked)
        result.push_back(entry.first);
    if (identity < 0 && wildcard != 0)
        result.emplace_back("");
    return result;
}

std::string format_http_date_gmt(std::time_t timestamp) {
    struct tm tm_value;
    char buffer[64];
    gmtime_r(&timestamp, &tm_value);
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT",
                  &tm_value);
    return buffer;
}

} // namespace zhttp
