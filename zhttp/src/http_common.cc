#include "http_common.h"

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

HttpMethod string_to_method(const std::string &str) {
  // 转换为大写进行比较
  std::string upper = str;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return std::toupper(c); });

  if (upper == "GET")
    return HttpMethod::GET;
  if (upper == "POST")
    return HttpMethod::POST;
  if (upper == "PUT")
    return HttpMethod::PUT;
  if (upper == "DELETE")
    return HttpMethod::DELETE;
  if (upper == "HEAD")
    return HttpMethod::HEAD;
  if (upper == "OPTIONS")
    return HttpMethod::OPTIONS;
  if (upper == "PATCH")
    return HttpMethod::PATCH;
  if (upper == "CONNECT")
    return HttpMethod::CONNECT;
  if (upper == "TRACE")
    return HttpMethod::TRACE;
  return HttpMethod::UNKNOWN;
}

const char *status_to_string(HttpStatus status) {
  switch (status) {
  // 1xx
  case HttpStatus::CONTINUE:
    return "Continue";
  case HttpStatus::SWITCHING_PROTOCOLS:
    return "Switching Protocols";
  // 2xx
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
  // 3xx
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
  // 4xx
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
  case HttpStatus::TOO_MANY_REQUESTS:
    return "Too Many Requests";
  // 5xx
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
    return "Unknown";
  }
}

const char *get_mime_type(const std::string &extension) {
  static const std::unordered_map<std::string, const char *> mime_types = {
      // Text
      {"html", "text/html"},
      {"htm", "text/html"},
      {"css", "text/css"},
      {"js", "application/javascript"},
      {"json", "application/json"},
      {"xml", "application/xml"},
      {"txt", "text/plain"},
      // Images
      {"png", "image/png"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},
      {"svg", "image/svg+xml"},
      {"ico", "image/x-icon"},
      {"webp", "image/webp"},
      // Audio/Video
      {"mp3", "audio/mpeg"},
      {"mp4", "video/mp4"},
      {"webm", "video/webm"},
      // Documents
      {"pdf", "application/pdf"},
      {"zip", "application/zip"},
      {"gz", "application/gzip"},
      // Fonts
      {"woff", "font/woff"},
      {"woff2", "font/woff2"},
      {"ttf", "font/ttf"},
      // Other
      {"wasm", "application/wasm"},
  };

  std::string lower = to_lower(extension);
  auto it = mime_types.find(lower);
  if (it != mime_types.end()) {
    return it->second;
  }
  return "application/octet-stream";
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

HttpVersion string_to_version(const std::string &str) {
  if (str == "HTTP/1.0") {
    return HttpVersion::HTTP_1_0;
  }
  if (str == "HTTP/1.1") {
    return HttpVersion::HTTP_1_1;
  }
  return HttpVersion::UNKNOWN;
}

std::string to_lower(const std::string &str) {
  std::string lower = str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

void trim(std::string &str) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };

  while (!str.empty() && is_space(static_cast<unsigned char>(str.front()))) {
    str.erase(str.begin());
  }
  while (!str.empty() && is_space(static_cast<unsigned char>(str.back()))) {
    str.pop_back();
  }
}

std::vector<std::string> split_string(const std::string &str, char delimiter) {
  std::vector<std::string> parts;

  size_t start = 0;
  while (start <= str.size()) {
    size_t pos = str.find(delimiter, start);
    if (pos == std::string::npos) {
      pos = str.size();
    }

    parts.push_back(str.substr(start, pos - start));

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

std::string url_decode(const std::string &str) {
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
        // 已消费当前位置和后两位，for 迭代再 +1 后刚好指向下一个未处理字符。
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

} // namespace zhttp
