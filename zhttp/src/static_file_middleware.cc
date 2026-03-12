#include "static_file_middleware.h"

#include "http_common.h"

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace zhttp {

StaticFileMiddleware::StaticFileMiddleware(Options options)
    : options_(std::move(options)),
      normalized_prefix_(normalize_prefix(options_.uri_prefix)) {}

StaticFileMiddleware::StaticFileMiddleware()
  : StaticFileMiddleware(Options()) {}

bool StaticFileMiddleware::before(const HttpRequest::ptr &request,
                                  HttpResponse &response) {
  // 总体流程（从上到下）：
  // 1) 过滤非静态路径/非法方法；
  // 2) 将 URL 路径映射为 document_root 下的相对路径并做安全校验；
  // 3) 处理目录与 index 文件回落；
  // 4) 根据 Accept-Encoding 构造候选编码，优先尝试内存缓存；
  // 5) 缓存未命中则回源磁盘，支持 .br/.gz/原文件；
  // 6) 处理 Last-Modified 条件请求；
  // 7) 返回 HEAD/GET 响应，并在成功时回填内存缓存。

  // 阶段 1：入口过滤（路径范围 + HTTP 方法）。
  // 仅拦截配置前缀下的请求，其他请求继续走业务路由。
  // `path` 是不含 query string 的请求路径（例如 /static/app.js）。
  const std::string &path = request->path();
  if (!should_handle_path(path)) {
    // 返回 true = 本中间件“不消费请求”，交给后续中间件/路由继续处理。
    return true;
  }

  // 静态资源只支持 GET/HEAD。
  if (request->method() != HttpMethod::GET && request->method() != HttpMethod::HEAD) {
    // 返回 false = 已经生成响应并终止后续路由。
    response.status(HttpStatus::METHOD_NOT_ALLOWED)
        .header("Allow", "GET, HEAD")
        .text("Method Not Allowed");
    return false;
  }

  // 阶段 2：URI -> 相对路径，并进行路径穿越防护。
  std::string relative_raw = map_to_relative_path(path);
  std::string relative;
  if (!sanitize_relative_path(relative_raw, relative)) {
    // 任何可疑路径（如 ..）都直接 403，避免访问 document_root 外部文件。
    response.status(HttpStatus::FORBIDDEN).text("Forbidden");
    return false;
  }

  // 阶段 3：拼接磁盘路径，并处理目录 index 回落。
  bool path_ends_with_slash = !relative_raw.empty() && relative_raw.back() == '/';
  std::string disk_path = join_path(options_.document_root, relative);

  if (is_directory(disk_path)) {
    // 命中真实目录：按配置决定是否自动补 index 文件。
    if (!options_.enable_implicit_index) {
      response.status(HttpStatus::FORBIDDEN).text("Forbidden");
      return false;
    }
    disk_path = join_path(disk_path, options_.index_file);
  } else if ((relative.empty() || path_ends_with_slash) && options_.enable_implicit_index) {
    // 目标不是目录，但路径语义上像“目录入口”（空路径或尾 '/'），也尝试 index 回落。
    disk_path = join_path(disk_path, options_.index_file);
  }

  // 阶段 4：编码协商（仅做“候选顺序”决策，真正选文件在后面）。
  std::vector<std::string> encoding_candidates;
  // 编码协商顺序：br -> gzip -> identity。
  if (options_.br_static && accepts_encoding(request, "br")) {
    encoding_candidates.push_back("br");
  }
  if (options_.gzip_static && accepts_encoding(request, "gzip")) {
    encoding_candidates.push_back("gzip");
  }
  encoding_candidates.push_back("");

  const std::string content_type = detect_content_type(disk_path);

  // 阶段 5：先查内存缓存（命中可直接返回，避免磁盘 I/O）。
  if (options_.enable_memory_cache && options_.memory_cache_time > 0) {
    // 缓存 key 由“原始请求路径 + 编码”构成，避免不同编码体互相污染。
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (const auto &enc : encoding_candidates) {
      const std::string cache_key = path + "|" + enc;
      auto it = cache_.find(cache_key);
      if (it == cache_.end()) {
        continue;
      }

      if (Clock::now() <= it->second.expires_at) {
        // 命中未过期缓存：优先走内存返回，不触发磁盘 stat/read。
        // 缓存命中后也要遵守条件请求：If-Modified-Since 一致则返回 304。
        if (options_.enable_last_modified) {
          const std::string ims = request->header("If-Modified-Since");
          if (!ims.empty() && ims == it->second.last_modified) {
            response.status(HttpStatus::NOT_MODIFIED);
            response.header("Last-Modified", it->second.last_modified);
            if (!options_.cache_control.empty()) {
              response.header("Cache-Control", options_.cache_control);
            }
            if (options_.gzip_static || options_.br_static) {
              response.header("Vary", "Accept-Encoding");
            }
            if (!it->second.content_encoding.empty()) {
              response.header("Content-Encoding", it->second.content_encoding);
            }
            response.set_keep_alive(request->is_keep_alive());
            return false;
          }
        }

        response.status(HttpStatus::OK).content_type(it->second.content_type);
        if (!it->second.content_encoding.empty()) {
          response.header("Content-Encoding", it->second.content_encoding);
        }
        if (!it->second.last_modified.empty()) {
          response.header("Last-Modified", it->second.last_modified);
        }
        if (!options_.cache_control.empty()) {
          response.header("Cache-Control", options_.cache_control);
        }
        if (options_.gzip_static || options_.br_static) {
          response.header("Vary", "Accept-Encoding");
        }

        if (request->method() == HttpMethod::HEAD) {
          // HEAD 只返回头部，不返回实体，但 Content-Length 要与 GET 一致。
          response.header("Content-Length", std::to_string(it->second.content_length));
          response.body("");
        } else {
          response.body(it->second.body);
        }
        response.set_keep_alive(request->is_keep_alive());
        return false;
      }

      // 过期缓存懒删除：仅在访问到该 key 时清理。
      cache_.erase(it);
    }
  }

  // 阶段 6：缓存未命中时，按候选顺序选择磁盘文件。
  // 注意：`content_type` 基于原始目标路径（不带 .br/.gz）推断，保证 MIME 正确。
  std::string content_encoding;
  std::string selected_path;
  for (const auto &enc : encoding_candidates) {
    // 按候选顺序短路：找到第一个存在文件即停止，确保协商顺序生效。
    if (enc == "br") {
      std::string br_path = disk_path + ".br";
      if (is_regular_file(br_path)) {
        selected_path = br_path;
        content_encoding = "br";
        break;
      }
      continue;
    }
    if (enc == "gzip") {
      std::string gzip_path = disk_path + ".gz";
      if (is_regular_file(gzip_path)) {
        selected_path = gzip_path;
        content_encoding = "gzip";
        break;
      }
      continue;
    }
    if (is_regular_file(disk_path)) {
      selected_path = disk_path;
      content_encoding.clear();
      break;
    }
  }

  if (selected_path.empty()) {
    // 中间件不强制 404，交给后续业务路由（可用于 SPA fallback 等场景）。
    return true;
  }

  // 阶段 7：磁盘回源命中后，处理 Last-Modified 条件请求。
  std::string last_modified;
  if (options_.enable_last_modified && get_last_modified(selected_path, last_modified)) {
    const std::string ims = request->header("If-Modified-Since");
    if (!ims.empty() && ims == last_modified) {
      // 条件命中：资源未变化，直接返回 304，避免发送包体。
      response.status(HttpStatus::NOT_MODIFIED);
      response.header("Last-Modified", last_modified);
      if (!options_.cache_control.empty()) {
        response.header("Cache-Control", options_.cache_control);
      }
      if (!content_encoding.empty()) {
        response.header("Content-Encoding", content_encoding);
      }
      if (options_.gzip_static || options_.br_static) {
        response.header("Vary", "Accept-Encoding");
      }
      response.set_keep_alive(request->is_keep_alive());
      return false;
    }
  }

  // 阶段 8：读取文件并组装响应（HEAD 不回包体，只回 Content-Length）。
  std::string content;
  if (!read_file(selected_path, content)) {
    // 文件在 stat/read 之间可能被删除或不可读，回退给后续路由统一处理。
    return true;
  }

  response.status(HttpStatus::OK).content_type(content_type);
  if (!content_encoding.empty()) {
    response.header("Content-Encoding", content_encoding);
  }
  if (!last_modified.empty()) {
    response.header("Last-Modified", last_modified);
  }
  if (!options_.cache_control.empty()) {
    response.header("Cache-Control", options_.cache_control);
  }
  if (options_.gzip_static || options_.br_static) {
    response.header("Vary", "Accept-Encoding");
  }

  if (request->method() == HttpMethod::HEAD) {
    response.header("Content-Length", std::to_string(content.size()));
    response.body("");
  } else {
    response.body(content);
  }
  response.set_keep_alive(request->is_keep_alive());

  // 阶段 9：响应成功后按配置写入内存缓存（只缓存小文件）。
  if (options_.enable_memory_cache && options_.memory_cache_time > 0 &&
      content.size() <= options_.max_cached_file_size) {
    // 缓存体与响应体一致；HEAD 请求也会缓存，便于后续 GET 复用。
    CacheEntry entry;
    entry.body = std::move(content);
    entry.content_type = content_type;
    entry.content_encoding = content_encoding;
    entry.last_modified = last_modified;
    entry.content_length = entry.body.size();
    entry.expires_at =
        Clock::now() + std::chrono::seconds(options_.memory_cache_time);
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[path + "|" + content_encoding] = std::move(entry);
  }

  return false;
}

void StaticFileMiddleware::after(const HttpRequest::ptr &, HttpResponse &) {}

bool StaticFileMiddleware::should_handle_path(const std::string &path) const {
  // 根前缀模式：只要是绝对路径都由该中间件接管。
  if (normalized_prefix_ == "/") {
    return !path.empty() && path[0] == '/';
  }

  if (path.size() < normalized_prefix_.size() ||
      path.compare(0, normalized_prefix_.size(), normalized_prefix_) != 0) {
    return false;
  }

  if (path.size() == normalized_prefix_.size()) {
    return true;
  }

  return path[normalized_prefix_.size()] == '/';
}

std::string StaticFileMiddleware::map_to_relative_path(const std::string &path) const {
  // 将请求路径去掉挂载前缀，转换为 document_root 下相对路径。
  // 例如：prefix=/static, path=/static/js/app.js => /js/app.js
  if (normalized_prefix_ == "/") {
    return path;
  }
  if (path.size() <= normalized_prefix_.size()) {
    return "/";
  }
  return path.substr(normalized_prefix_.size());
}

bool StaticFileMiddleware::sanitize_relative_path(const std::string &raw,
                                                  std::string &out) const {
  // 输入约定：raw 通常形如 "/a/b" 或 "/"，可能包含 URL 编码字符。
  // 目标：输出“安全的相对路径”（不含前导 '/'），用于拼接 document_root。
  //
  // 处理策略：
  // 1) 先 URL 解码，避免 ".." 被 %2e%2e 这类形式绕过；
  // 2) 按 '/' 分段并做归一化；
  // 3) 忽略空段和 "."，遇到 ".." 立即拒绝（防目录穿越）；
  // 4) 将合法段重新用 '/' 连接。
  std::string decoded = url_decode(raw);
  std::vector<std::string> segments;
  std::string segment;

  auto flush_segment = [&segments](std::string &seg) -> bool {
    // 段归一化规则：
    // - 空段：由连续 '/' 或首尾 '/' 产生，忽略；
    // - "."：当前目录语义，忽略；
    // - ".."：上级目录语义，直接拒绝（返回 false）；
    // - 其他：视为普通路径段，压入结果。
    if (seg.empty() || seg == ".") {
      seg.clear();
      return true;
    }
    if (seg == "..") {
      return false;
    }
    segments.push_back(seg);
    seg.clear();
    return true;
  };

  // 单次线性扫描 decoded：
  // - 遇到 '/' 说明当前段结束，触发 flush；
  // - 非 '/' 字符累积到当前段。
  for (char c : decoded) {
    if (c == '/') {
      if (!flush_segment(segment)) {
        return false;
      }
      continue;
    }
    segment.push_back(c);
  }

  // 别漏掉最后一段（字符串可能不以 '/' 结尾）。
  if (!flush_segment(segment)) {
    return false;
  }

  // 将合法段重建为规范相对路径：
  // segments = ["a", "b"] => "a/b"；
  // segments 为空 => ""（调用方会按目录/index 规则继续处理）。
  std::ostringstream oss;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      oss << '/';
    }
    oss << segments[i];
  }
  out = oss.str();
  return true;
}

std::string StaticFileMiddleware::join_path(const std::string &left,
                                            const std::string &right) const {
  // 路径拼接规则：
  // - 任一侧为空时返回另一侧；
  // - 已有尾 '/' 则直接拼接；
  // - 否则中间补一个 '/'.
  if (right.empty()) {
    return left;
  }
  if (left.empty()) {
    return right;
  }
  if (left.back() == '/') {
    return left + right;
  }
  return left + "/" + right;
}

bool StaticFileMiddleware::is_regular_file(const std::string &path) const {
  // 使用 stat 判断是否存在且为普通文件。
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISREG(st.st_mode);
}

bool StaticFileMiddleware::is_directory(const std::string &path) const {
  // 使用 stat 判断是否存在且为目录。
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

bool StaticFileMiddleware::read_file(const std::string &path,
                                     std::string &content) const {
  // 二进制方式读取，避免文本模式下换行或编码转换影响内容。
  std::ifstream ifs(path, std::ios::in | std::ios::binary);
  if (!ifs) {
    return false;
  }
  // 通过流缓冲一次性读入字符串，适合中小静态文件。
  std::ostringstream oss;
  oss << ifs.rdbuf();
  content = oss.str();
  return true;
}

std::string StaticFileMiddleware::normalize_prefix(const std::string &prefix) const {
  // 统一前缀格式：
  // - 空串/"/" 归一为 "/"
  // - 确保以 '/' 开头
  // - 去掉末尾多余 '/'
  if (prefix.empty() || prefix == "/") {
    return "/";
  }

  std::string value = prefix;
  if (value[0] != '/') {
    value = "/" + value;
  }
  while (value.size() > 1 && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string StaticFileMiddleware::detect_content_type(const std::string &file_path) const {
  // 取最后一个 '.' 后的扩展名映射 MIME。
  // 若无扩展名或扩展名为空，回退到默认二进制类型。
  size_t dot = file_path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= file_path.size()) {
    return get_mime_type("");
  }
  return get_mime_type(file_path.substr(dot + 1));
}

std::string StaticFileMiddleware::format_http_date(time_t timestamp) const {
  // 统一转为 RFC 7231 兼容的 GMT 时间字符串。
  return format_http_date_gmt(timestamp);
}

bool StaticFileMiddleware::accepts_encoding(const HttpRequest::ptr &request,
                                            const std::string &encoding) const {
  // 简化匹配：将请求头与目标编码统一为小写后做子串查找。
  // 该实现追求轻量，未解析 q 值优先级。
  std::string header = to_lower(request->header("Accept-Encoding"));
  std::string token = to_lower(encoding);
  return header.find(token) != std::string::npos;
}

bool StaticFileMiddleware::get_last_modified(const std::string &path,
                                             std::string &last_modified) const {
  // 读取文件 mtime，并格式化为 HTTP 响应头可直接使用的日期。
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  last_modified = format_http_date(st.st_mtime);
  return true;
}

} // namespace zhttp
