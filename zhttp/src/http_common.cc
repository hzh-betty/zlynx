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

  // 转换为小写
  std::string lower = extension;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

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

} // namespace zhttp
