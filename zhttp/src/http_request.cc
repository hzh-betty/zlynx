#include "http_request.h"

#include "http_common.h"
#include "multipart.h"

#include <cstdlib>

namespace zhttp {

namespace {
static std::string normalize_mime_type(const std::string &content_type) {
  std::string mime = content_type;
  size_t semi = mime.find(';');
  if (semi != std::string::npos) {
    mime = mime.substr(0, semi);
  }
  trim(mime);
  return to_lower(mime);
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
void HttpRequest::parse_cookies_if_needed() {
  if (runtime_.cookies_parsed) {
    return;
  }

  // 先置位，保证即使头不存在也不会重复进入解析流程。
  runtime_.cookies_parsed = true;
  std::string cookie_header = header("Cookie");
  if (cookie_header.empty()) {
    runtime_.cookies.clear();
    return;
  }

  parse_cookie_header(cookie_header, runtime_.cookies);
}

// 读取单个 Cookie，未命中时返回调用方给定的默认值。
std::string HttpRequest::cookie(const std::string &key,
                                const std::string &default_val) const {
  const_cast<HttpRequest *>(this)->parse_cookies_if_needed();
  auto it = runtime_.cookies.find(key);
  if (it != runtime_.cookies.end()) {
    return it->second;
  }
  return default_val;
}

// 返回全部 Cookie；这里同样依赖延迟解析。
const HttpRequest::Params &HttpRequest::cookies() const {
  const_cast<HttpRequest *>(this)->parse_cookies_if_needed();
  return runtime_.cookies;
}

// 头字段按原始 key 保存，读取时再做大小写不敏感匹配。
void HttpRequest::set_header(const std::string &key, const std::string &value) {
  headers_[key] = value;

  // 影响请求体解析语义的头变化后，清理缓存，避免旧结果污染。
  if (to_lower(key) == "content-type") {
    invalidate_body_cache();
  }
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

// 仅判断主 MIME 是否为 application/json，忽略 charset 等附加参数。
bool HttpRequest::is_json() const {
  return normalize_mime_type(content_type()) == "application/json";
}

/**
 * 解析 JSON 请求体。
 *
 * 语义约定：
 * - 非 JSON 请求直接返回 true（表示“无需解析”，不是错误）；
 * - 空 JSON Body 视为错误，返回 false 并记录 json_error_；
 * - 非法 JSON 文本返回 false 并记录 json_error_；
 * - 成功时缓存结果，后续重复调用不重复解析。
 */
bool HttpRequest::parse_json() {
  // 已经解析过则直接复用缓存结果。
  if (runtime_.json_parsed) {
    return runtime_.json != nullptr;
  }

  // 标记已解析并清理旧状态，开始新一轮解析。
  runtime_.json_parsed = true;
  runtime_.json.reset();
  runtime_.json_error.clear();

  // 不是 JSON 请求：按“无需解析”处理。
  if (!is_json()) {
    return true;
  }

  // JSON 请求但 body 为空，显式给出错误。
  if (body_.empty()) {
    runtime_.json_error = "Empty JSON body";
    return false;
  }

  // 使用 nlohmann::json 非异常模式解析，失败时通过 is_discarded() 判断。
  Json parsed = Json::parse(body_, nullptr, false);
  if (parsed.is_discarded()) {
    runtime_.json_error = "Invalid JSON body";
    return false;
  }

  runtime_.json = std::make_shared<Json>(std::move(parsed));
  return true;
}

// const 场景下也允许触发惰性解析，因此通过 const_cast 复用实现。
const HttpRequest::Json *HttpRequest::json() const {
  if (!runtime_.json_parsed) {
    const_cast<HttpRequest *>(this)->parse_json();
  }
  return runtime_.json.get();
}

// 判断是否为 URL 编码表单请求体。
bool HttpRequest::is_form_urlencoded() const {
  return normalize_mime_type(content_type()) ==
         "application/x-www-form-urlencoded";
}

/**
 * 解析 application/x-www-form-urlencoded 请求体。
 *
 * 语义约定：
 * - 非该类型请求返回 true（无需解析）；
 * - 空 body 返回 true（结果为空表）；
 * - 同名 key 采用“后写覆盖前写”；
 * - 无等号片段记为空字符串，例如 "flag" -> flag=""。
 */
bool HttpRequest::parse_form_urlencoded() {
  // 表单解析结果无失败分支，解析过后直接复用。
  if (runtime_.form_parsed) {
    return true;
  }

  runtime_.form_parsed = true;
  runtime_.form_params.clear();

  if (!is_form_urlencoded()) {
    return true;
  }

  if (body_.empty()) {
    return true;
  }

  size_t pos = 0;
  while (pos < body_.size()) {
    // 每个片段以 '&' 分隔。
    size_t end = body_.find('&', pos);
    if (end == std::string::npos) {
      end = body_.size();
    }

    std::string pair = body_.substr(pos, end - pos);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      // key/value 均执行 URL 解码（包含 '+' -> 空格）。
      std::string key = url_decode(pair.substr(0, eq));
      std::string value = url_decode(pair.substr(eq + 1));
      runtime_.form_params[key] = value;
    } else if (!pair.empty()) {
      // 仅 key 无 value 的场景，统一记为空字符串。
      runtime_.form_params[url_decode(pair)] = "";
    }

    pos = end + 1;
  }

  return true;
}

// 惰性读取表单字段，首次访问时自动触发解析。
const HttpRequest::Params &HttpRequest::form_params() const {
  if (!runtime_.form_parsed) {
    const_cast<HttpRequest *>(this)->parse_form_urlencoded();
  }
  return runtime_.form_params;
}

// 按 key 读取表单字段，未命中返回默认值。
std::string HttpRequest::form_param(const std::string &key,
                                    const std::string &default_val) const {
  const auto &fields = form_params();
  auto it = fields.find(key);
  if (it != fields.end()) {
    return it->second;
  }
  return default_val;
}

// multipart/form-data 常用于文件上传，这里只做快速识别，不负责真正解析。
bool HttpRequest::is_multipart() const {
  return normalize_mime_type(content_type()) == "multipart/form-data";
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
  if (runtime_.multipart_parsed) {
    return runtime_.multipart != nullptr;
  }

  // 先清理旧状态，再开始新一轮解析。
  runtime_.multipart_parsed = true;
  runtime_.multipart.reset();
  runtime_.multipart_error.clear();

  if (!is_multipart()) {
    return true;
  }

  auto parsed = MultipartFormData::parse(*this, &runtime_.multipart_error);
  if (!parsed) {
    return false;
  }
  runtime_.multipart = std::move(parsed);
  return true;
}

// const 接口也允许触发懒解析，因此这里通过 const_cast 复用已有实现。
const MultipartFormData *HttpRequest::multipart() const {
  if (!runtime_.multipart_parsed) {
    const_cast<HttpRequest *>(this)->parse_multipart();
  }
  return runtime_.multipart.get();
}

void HttpRequest::invalidate_body_cache() {
  runtime_.multipart_parsed = false;
  runtime_.multipart.reset();
  runtime_.multipart_error.clear();

  runtime_.json_parsed = false;
  runtime_.json.reset();
  runtime_.json_error.clear();

  runtime_.form_parsed = false;
  runtime_.form_params.clear();
}

} // namespace zhttp
