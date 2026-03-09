#include "http_request.h"

#include "multipart.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace zhttp {

namespace {
// 将字符串转换为小写（用于大小写不敏感比较）
std::string to_lower(const std::string &str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

// URL 解码
std::string url_decode(const std::string &str) {
  std::string result;
  result.reserve(str.size());

  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      int high = std::isxdigit(str[i + 1]) ? str[i + 1] : 0;
      int low = std::isxdigit(str[i + 2]) ? str[i + 2] : 0;
      if (high && low) {
        auto hex_to_int = [](char c) -> int {
          if (c >= '0' && c <= '9')
            return c - '0';
          if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
          if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
          return 0;
        };
        result += static_cast<char>(hex_to_int(str[i + 1]) * 16 +
                                    hex_to_int(str[i + 2]));
        i += 2;
        continue;
      }
    } else if (str[i] == '+') {
      result += ' ';
      continue;
    }
    result += str[i];
  }
  return result;
}

static inline void trim(std::string &s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
}

static void parse_cookie_header(const std::string &cookie_header,
                                HttpRequest::Params &out) {
  out.clear();
  size_t pos = 0;
  while (pos < cookie_header.size()) {
    size_t end = cookie_header.find(';', pos);
    if (end == std::string::npos) {
      end = cookie_header.size();
    }

    std::string token = cookie_header.substr(pos, end - pos);
    trim(token);
    if (!token.empty()) {
      size_t eq = token.find('=');
      if (eq != std::string::npos) {
        std::string key = token.substr(0, eq);
        std::string value = token.substr(eq + 1);
        trim(key);
        trim(value);
        if (!key.empty()) {
          out[key] = value;
        }
      } else {
        out[token] = "";
      }
    }

    pos = end + 1;
  }
}
} // namespace

std::string HttpRequest::header(const std::string &key,
                                const std::string &default_val) const {
  std::string lower_key = to_lower(key);
  for (const auto &pair : headers_) {
    if (to_lower(pair.first) == lower_key) {
      return pair.second;
    }
  }
  return default_val;
}

std::string HttpRequest::path_param(const std::string &key,
                                    const std::string &default_val) const {
  auto it = path_params_.find(key);
  if (it != path_params_.end()) {
    return it->second;
  }
  return default_val;
}

std::string HttpRequest::query_param(const std::string &key,
                                     const std::string &default_val) const {
  auto it = query_params_.find(key);
  if (it != query_params_.end()) {
    return it->second;
  }
  return default_val;
}

void HttpRequest::parse_cookies_if_needed() const {
  if (cookies_parsed_) {
    return;
  }

  cookies_parsed_ = true;
  std::string cookie_header = header("Cookie");
  if (cookie_header.empty()) {
    cookies_.clear();
    return;
  }

  parse_cookie_header(cookie_header, cookies_);
}

std::string HttpRequest::cookie(const std::string &key,
                                const std::string &default_val) const {
  parse_cookies_if_needed();
  auto it = cookies_.find(key);
  if (it != cookies_.end()) {
    return it->second;
  }
  return default_val;
}

const HttpRequest::Params &HttpRequest::cookies() const {
  parse_cookies_if_needed();
  return cookies_;
}

void HttpRequest::set_header(const std::string &key, const std::string &value) {
  headers_[key] = value;
}

void HttpRequest::set_path_param(const std::string &key,
                                 const std::string &value) {
  path_params_[key] = value;
}

void HttpRequest::parse_query_params() {
  query_params_.clear();
  if (query_.empty()) {
    return;
  }

  size_t pos = 0;
  while (pos < query_.size()) {
    // 找到 & 或结尾
    size_t end = query_.find('&', pos);
    if (end == std::string::npos) {
      end = query_.size();
    }

    // 提取键值对
    std::string pair = query_.substr(pos, end - pos);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      std::string key = url_decode(pair.substr(0, eq));
      std::string value = url_decode(pair.substr(eq + 1));
      query_params_[key] = value;
    } else if (!pair.empty()) {
      query_params_[url_decode(pair)] = "";
    }

    pos = end + 1;
  }
}

bool HttpRequest::is_keep_alive() const {
  std::string connection = header("Connection");
  if (version_ == HttpVersion::HTTP_1_1) {
    // HTTP/1.1 默认为 keep-alive
    return to_lower(connection) != "close";
  }
  // HTTP/1.0 默认关闭
  return to_lower(connection) == "keep-alive";
}

size_t HttpRequest::content_length() const {
  std::string len_str = header("Content-Length");
  if (len_str.empty()) {
    return 0;
  }
  return static_cast<size_t>(std::strtoul(len_str.c_str(), nullptr, 10));
}

std::string HttpRequest::content_type() const { return header("Content-Type"); }

bool HttpRequest::is_multipart() const {
  std::string ct = content_type();
  return to_lower(ct).find("multipart/form-data") != std::string::npos;
}

bool HttpRequest::parse_multipart() {
  if (multipart_parsed_) {
    return multipart_ != nullptr;
  }
  multipart_parsed_ = true;
  multipart_.reset();
  multipart_error_.clear();

  if (!is_multipart()) {
    return true;
  }

  auto parsed = MultipartFormData::parse(*this, &multipart_error_);
  if (!parsed) {
    return false;
  }
  multipart_ = std::move(parsed);
  return true;
}

const MultipartFormData *HttpRequest::multipart() const {
  if (!multipart_parsed_) {
    const_cast<HttpRequest *>(this)->parse_multipart();
  }
  return multipart_.get();
}

} // namespace zhttp
