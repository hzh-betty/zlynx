#include "http_request.h"

#include "multipart.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace zhttp {

namespace {
/**
 * HTTP 头字段名、Connection 等协议字段通常需要大小写不敏感比较。
 * 这里不直接修改原始字符串，而是复制一份再统一转成小写，方便后续比较。
 */
std::string to_lower(const std::string &str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

/**
 * 对 URL 编码字符串做解码。
 *
 * 这里主要处理两类常见编码：
 * 1. %XX 形式的十六进制转义，例如 %20 -> 空格。
 * 2. 查询字符串中的 +，按 application/x-www-form-urlencoded 语义解码为空格。
 *
 * 如果遇到非法的 % 编码，例如后面不足两个字符，或者不是十六进制字符，
 * 当前实现会保留原字符，不会抛异常，这样可以避免把一条坏请求放大成崩溃。
 */
std::string url_decode(const std::string &str) {
  std::string result;
  result.reserve(str.size());

  for (size_t i = 0; i < str.size(); ++i) {
    // 处理 %XX 形式的编码。
    if (str[i] == '%' && i + 2 < str.size()) {
      int high = std::isxdigit(str[i + 1]) ? str[i + 1] : 0;
      int low = std::isxdigit(str[i + 2]) ? str[i + 2] : 0;
      if (high && low) {
        // 把单个十六进制字符转成整数值。
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
      // 表单编码里 + 表示空格。
      result += ' ';
      continue;
    }
    // 其他字符原样拷贝。
    result += str[i];
  }
  return result;
}

// 去掉字符串首尾空白，便于解析 header/cookie 中的 token。
static inline void trim(std::string &s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
}

/**
 * 解析 Cookie 请求头。
 *
 * 典型输入："a=1; theme=dark; flag"
 * 解析结果：
 * - a -> 1
 * - theme -> dark
 * - flag -> ""
 *
 * 注意这里解析的是请求头里的 Cookie，而不是响应头里的 Set-Cookie，
 * 两者语义和格式并不完全一样。
 */
static void parse_cookie_header(const std::string &cookie_header,
                                HttpRequest::Params &out) {
  // 每次解析前清空输出，避免重复调用时残留旧数据。
  out.clear();
  size_t pos = 0;
  while (pos < cookie_header.size()) {
    // 一个 cookie 项通常由分号分隔。
    size_t end = cookie_header.find(';', pos);
    if (end == std::string::npos) {
      end = cookie_header.size();
    }

    std::string token = cookie_header.substr(pos, end - pos);
    trim(token);
    if (!token.empty()) {
      // key=value 是最常见格式；某些场景下也可能只有 key。
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
        // 没有等号时仍然保留这个标记，值置空。
        out[token] = "";
      }
    }

    // 跳到下一个 token 的起始位置。
    pos = end + 1;
  }
}
} // namespace

/**
 * 大小写不敏感地读取请求头。
 *
 * 虽然 headers_ 当前按原始 key 保存，但 HTTP 头字段名本身不区分大小写，
 * 因此这里逐项转小写比较，保证 Header、header、HEADER 都能命中。
 */
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

// 路由匹配阶段提取出的路径参数，按 key 直接查询即可。
std::string HttpRequest::path_param(const std::string &key,
                                    const std::string &default_val) const {
  auto it = path_params_.find(key);
  if (it != path_params_.end()) {
    return it->second;
  }
  return default_val;
}

// 查询参数已经在 parse_query_params 中标准化，这里只做简单查表。
std::string HttpRequest::query_param(const std::string &key,
                                     const std::string &default_val) const {
  auto it = query_params_.find(key);
  if (it != query_params_.end()) {
    return it->second;
  }
  return default_val;
}

/**
 * 按需解析 Cookie。
 *
 * 不是所有请求都会访问 Cookie，因此这里采用延迟解析：
 * 只有第一次调用 cookie()/cookies() 时才真正解析请求头。
 * 这样可以减少不必要的字符串处理开销。
 */
void HttpRequest::parse_cookies_if_needed() const {
  if (cookies_parsed_) {
    return;
  }

  // 先置位，保证即使头不存在也不会重复进入解析流程。
  cookies_parsed_ = true;
  std::string cookie_header = header("Cookie");
  if (cookie_header.empty()) {
    cookies_.clear();
    return;
  }

  parse_cookie_header(cookie_header, cookies_);
}

// 读取单个 Cookie，未命中时返回调用方给定的默认值。
std::string HttpRequest::cookie(const std::string &key,
                                const std::string &default_val) const {
  parse_cookies_if_needed();
  auto it = cookies_.find(key);
  if (it != cookies_.end()) {
    return it->second;
  }
  return default_val;
}

// 返回全部 Cookie；这里同样依赖延迟解析。
const HttpRequest::Params &HttpRequest::cookies() const {
  parse_cookies_if_needed();
  return cookies_;
}

// 头字段按原始 key 保存，读取时再做大小写不敏感匹配。
void HttpRequest::set_header(const std::string &key, const std::string &value) {
  headers_[key] = value;
}

// 路径参数通常由路由器写入，这里只是简单落盘。
void HttpRequest::set_path_param(const std::string &key,
                                 const std::string &value) {
  path_params_[key] = value;
}

/**
 * 解析 URL 中 ? 后面的查询字符串。
 *
 * 例如：name=betty&debug=true&empty
 * 解析后得到：
 * - name -> betty
 * - debug -> true
 * - empty -> ""
 */
void HttpRequest::parse_query_params() {
  // 重新解析前先清空，避免请求对象复用时保留旧值。
  query_params_.clear();
  if (query_.empty()) {
    return;
  }

  size_t pos = 0;
  while (pos < query_.size()) {
    // 每个参数对通常由 & 分隔。
    size_t end = query_.find('&', pos);
    if (end == std::string::npos) {
      end = query_.size();
    }

    // 先切出一个完整片段，再看是否存在等号。
    std::string pair = query_.substr(pos, end - pos);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      // key 和 value 都需要 URL 解码。
      std::string key = url_decode(pair.substr(0, eq));
      std::string value = url_decode(pair.substr(eq + 1));
      query_params_[key] = value;
    } else if (!pair.empty()) {
      // 只有 key 没有 value 的场景，统一记为空字符串。
      query_params_[url_decode(pair)] = "";
    }

    pos = end + 1;
  }
}

/**
 * 判断连接是否应保持长连接。
 *
 * 规则来自 HTTP 协议版本约定：
 * - HTTP/1.1 默认长连接，除非显式写 Connection: close
 * - HTTP/1.0 默认短连接，只有写 Connection: keep-alive 才保持
 */
bool HttpRequest::is_keep_alive() const {
  std::string connection = header("Connection");
  if (version_ == HttpVersion::HTTP_1_1) {
    // HTTP/1.1 默认为 keep-alive
    return to_lower(connection) != "close";
  }
  // HTTP/1.0 默认关闭
  return to_lower(connection) == "keep-alive";
}

// 从请求头读取 Content-Length；缺失时按 0 处理。
size_t HttpRequest::content_length() const {
  std::string len_str = header("Content-Length");
  if (len_str.empty()) {
    return 0;
  }
  // 这里使用 strtoul，非法内容会退化为 0，调用方应结合协议校验整体合法性。
  return static_cast<size_t>(std::strtoul(len_str.c_str(), nullptr, 10));
}

// 直接返回原始 Content-Type 字段，便于上层自己决定是否进一步解析参数。
std::string HttpRequest::content_type() const { return header("Content-Type"); }

// multipart/form-data 常用于文件上传，这里只做快速识别，不负责真正解析。
bool HttpRequest::is_multipart() const {
  std::string ct = content_type();
  return to_lower(ct).find("multipart/form-data") != std::string::npos;
}

/**
 * 解析 multipart/form-data 请求体。
 *
 * 这里同样采用延迟解析并带缓存：
 * - 如果已经解析过，直接复用上次结果
 * - 如果当前请求不是 multipart，视为无需解析，返回 true
 * - 真正的边界解析逻辑交给 MultipartFormData::parse
 */
bool HttpRequest::parse_multipart() {
  if (multipart_parsed_) {
    return multipart_ != nullptr;
  }

  // 先清理旧状态，再开始新一轮解析。
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

// const 接口也允许触发懒解析，因此这里通过 const_cast 复用已有实现。
const MultipartFormData *HttpRequest::multipart() const {
  if (!multipart_parsed_) {
    const_cast<HttpRequest *>(this)->parse_multipart();
  }
  return multipart_.get();
}

} // namespace zhttp
