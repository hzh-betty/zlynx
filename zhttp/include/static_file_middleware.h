#ifndef ZHTTP_STATIC_FILE_MIDDLEWARE_H_
#define ZHTTP_STATIC_FILE_MIDDLEWARE_H_

#include "middleware.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zhttp {

class StaticFileMiddleware : public Middleware {
public:
  using Clock = std::chrono::steady_clock;

  struct Options {
    Options()
        : uri_prefix("/"),
          document_root("."),
          index_file("index.html"),
          cache_control("public, max-age=60"),
          enable_implicit_index(true),
          enable_last_modified(true),
          enable_memory_cache(true),
          gzip_static(true),
          br_static(true),
          memory_cache_time(5),
          max_cached_file_size(1024 * 1024) {}

    std::string uri_prefix;
    std::string document_root;
    std::string index_file;
    std::string cache_control;
    bool enable_implicit_index;
    bool enable_last_modified;
    bool enable_memory_cache;
    bool gzip_static;
    bool br_static;
    int memory_cache_time;
    size_t max_cached_file_size;
  };

  StaticFileMiddleware();
  explicit StaticFileMiddleware(Options options);

  bool before(const HttpRequest::ptr &request, HttpResponse &response) override;
  void after(const HttpRequest::ptr &request, HttpResponse &response) override;

private:
  struct CacheEntry {
    std::string body;
    std::string content_type;
    std::string content_encoding;
    std::string last_modified;
    size_t content_length = 0;
    Clock::time_point expires_at;
  };

  bool should_handle_path(const std::string &path) const;
  std::string map_to_relative_path(const std::string &path) const;
  bool sanitize_relative_path(const std::string &raw, std::string &out) const;
  std::string join_path(const std::string &left, const std::string &right) const;
  bool is_regular_file(const std::string &path) const;
  bool is_directory(const std::string &path) const;
  bool read_file(const std::string &path, std::string &content) const;
  std::string normalize_prefix(const std::string &prefix) const;
  std::string detect_content_type(const std::string &file_path) const;
  std::string format_http_date(time_t timestamp) const;
  bool accepts_encoding(const HttpRequest::ptr &request,
                        const std::string &encoding) const;
  bool get_last_modified(const std::string &path, std::string &last_modified) const;

private:
  Options options_;
  std::string normalized_prefix_;
  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::string, CacheEntry> cache_;
};

} // namespace zhttp

#endif // ZHTTP_STATIC_FILE_MIDDLEWARE_H_
