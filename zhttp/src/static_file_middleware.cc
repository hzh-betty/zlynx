#include "static_file_middleware.h"

#include "http_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace zhttp {

namespace {

std::string to_lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string url_decode(const std::string &input) {
  std::string output;
  output.reserve(input.size());

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

  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int hi = hex_to_int(input[i + 1]);
      int lo = hex_to_int(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        output.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    output.push_back(input[i]);
  }

  return output;
}

} // namespace

StaticFileMiddleware::StaticFileMiddleware(Options options)
    : options_(std::move(options)),
      normalized_prefix_(normalize_prefix(options_.uri_prefix)) {}

StaticFileMiddleware::StaticFileMiddleware()
  : StaticFileMiddleware(Options()) {}

bool StaticFileMiddleware::before(const HttpRequest::ptr &request,
                                  HttpResponse &response) {
  const std::string &path = request->path();
  if (!should_handle_path(path)) {
    return true;
  }

  if (request->method() != HttpMethod::GET && request->method() != HttpMethod::HEAD) {
    response.status(HttpStatus::METHOD_NOT_ALLOWED)
        .header("Allow", "GET, HEAD")
        .text("Method Not Allowed");
    return false;
  }

  std::string relative_raw = map_to_relative_path(path);
  std::string relative;
  if (!sanitize_relative_path(relative_raw, relative)) {
    response.status(HttpStatus::FORBIDDEN).text("Forbidden");
    return false;
  }

  bool path_ends_with_slash = !relative_raw.empty() && relative_raw.back() == '/';
  std::string disk_path = join_path(options_.document_root, relative);

  if (is_directory(disk_path)) {
    if (!options_.enable_implicit_index) {
      response.status(HttpStatus::FORBIDDEN).text("Forbidden");
      return false;
    }
    disk_path = join_path(disk_path, options_.index_file);
  } else if ((relative.empty() || path_ends_with_slash) && options_.enable_implicit_index) {
    disk_path = join_path(disk_path, options_.index_file);
  }

  std::vector<std::string> encoding_candidates;
  if (options_.br_static && accepts_encoding(request, "br")) {
    encoding_candidates.push_back("br");
  }
  if (options_.gzip_static && accepts_encoding(request, "gzip")) {
    encoding_candidates.push_back("gzip");
  }
  encoding_candidates.push_back("");

  const std::string content_type = detect_content_type(disk_path);

  if (options_.enable_memory_cache && options_.memory_cache_time > 0) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (const auto &enc : encoding_candidates) {
      const std::string cache_key = path + "|" + enc;
      auto it = cache_.find(cache_key);
      if (it == cache_.end()) {
        continue;
      }

      if (Clock::now() <= it->second.expires_at) {
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
          response.header("Content-Length", std::to_string(it->second.content_length));
          response.body("");
        } else {
          response.body(it->second.body);
        }
        response.set_keep_alive(request->is_keep_alive());
        return false;
      }

      cache_.erase(it);
    }
  }

  std::string content_encoding;
  std::string selected_path;
  for (const auto &enc : encoding_candidates) {
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
    return true;
  }

  std::string last_modified;
  if (options_.enable_last_modified && get_last_modified(selected_path, last_modified)) {
    const std::string ims = request->header("If-Modified-Since");
    if (!ims.empty() && ims == last_modified) {
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

  std::string content;
  if (!read_file(selected_path, content)) {
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

  if (options_.enable_memory_cache && options_.memory_cache_time > 0 &&
      content.size() <= options_.max_cached_file_size) {
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
  std::string decoded = url_decode(raw);
  std::vector<std::string> segments;
  std::string segment;

  auto flush_segment = [&segments](std::string &seg) -> bool {
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

  for (char c : decoded) {
    if (c == '/') {
      if (!flush_segment(segment)) {
        return false;
      }
      continue;
    }
    segment.push_back(c);
  }
  if (!flush_segment(segment)) {
    return false;
  }

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
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISREG(st.st_mode);
}

bool StaticFileMiddleware::is_directory(const std::string &path) const {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

bool StaticFileMiddleware::read_file(const std::string &path,
                                     std::string &content) const {
  std::ifstream ifs(path, std::ios::in | std::ios::binary);
  if (!ifs) {
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  content = oss.str();
  return true;
}

std::string StaticFileMiddleware::normalize_prefix(const std::string &prefix) const {
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
  size_t dot = file_path.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= file_path.size()) {
    return get_mime_type("");
  }
  return get_mime_type(file_path.substr(dot + 1));
}

std::string StaticFileMiddleware::format_http_date(time_t timestamp) const {
  struct tm tm_value;
  char buffer[64];
#ifdef _WIN32
  gmtime_s(&tm_value, &timestamp);
#else
  gmtime_r(&timestamp, &tm_value);
#endif
  std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm_value);
  return buffer;
}

bool StaticFileMiddleware::accepts_encoding(const HttpRequest::ptr &request,
                                            const std::string &encoding) const {
  std::string header = to_lower_copy(request->header("Accept-Encoding"));
  std::string token = to_lower_copy(encoding);
  return header.find(token) != std::string::npos;
}

bool StaticFileMiddleware::get_last_modified(const std::string &path,
                                             std::string &last_modified) const {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }
  last_modified = format_http_date(st.st_mtime);
  return true;
}

} // namespace zhttp
