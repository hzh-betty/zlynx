#ifndef ZHTTP_STATIC_FILE_MIDDLEWARE_H_
#define ZHTTP_STATIC_FILE_MIDDLEWARE_H_

#include "middleware.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief 静态文件分发中间件
 * @details
 * 该中间件用于在路由前快速处理静态资源请求，支持：
 * - URL 前缀映射与目录索引；
 * - 预压缩文件优先分发（.br / .gz）；
 * - `Last-Modified` 条件请求（304）；
 * - `ETag` / `If-None-Match` 条件请求（304）；
 * - 短期内存缓存，降低磁盘 I/O。
 *
 * 线程安全说明：
 * - 配置在构造后只读；
 * - 运行期仅缓存 map 需要并发保护，内部通过互斥锁实现。
 */
class StaticFileMiddleware : public Middleware {
public:
  using Clock = std::chrono::steady_clock;

  /**
   * @brief 静态文件中间件配置项
   */
  struct Options {
    Options()
        : uri_prefix("/"),
          document_root("."),
          index_file("index.html"),
          cache_control("public, max-age=60"),
          enable_implicit_index(true),
          enable_last_modified(true),
          enable_etag(true),
          enable_memory_cache(true),
          gzip_static(true),
          br_static(true),
          memory_cache_time(5),
          max_cached_file_size(1024 * 1024) {}

    std::string uri_prefix;         // 需要拦截的 URL 前缀，例如 /assets
    std::string document_root;      // 静态资源根目录
    std::string index_file;         // 目录请求默认文件名，例如 index.html
    std::string cache_control;      // 回写到响应中的 Cache-Control
    bool enable_implicit_index;     // 是否允许目录自动补 index 文件
    bool enable_last_modified;      // 是否启用 Last-Modified / 304 逻辑
    bool enable_etag;               // 是否启用 ETag / If-None-Match 逻辑
    bool enable_memory_cache;       // 是否启用内存缓存
    bool gzip_static;               // 是否启用 .gz 预压缩文件分发
    bool br_static;                 // 是否启用 .br 预压缩文件分发
    int memory_cache_time;          // 内存缓存 TTL（秒）
    size_t max_cached_file_size;    // 允许进入内存缓存的最大文件体积（字节）
  };

  /**
   * @brief 使用默认配置构造静态文件中间件
   */
  StaticFileMiddleware();

  /**
   * @brief 使用自定义配置构造静态文件中间件
   * @param options 中间件配置
   */
  explicit StaticFileMiddleware(Options options);

  /**
   * @brief 请求前置处理
   * @return true 继续走后续中间件/路由；false 表示已完成响应并终止链路
   */
  bool before(const HttpRequest::ptr &request, HttpResponse &response) override;

  /**
   * @brief 请求后置处理
   * @details 当前实现无需额外收尾逻辑。
   */
  void after(const HttpRequest::ptr &request, HttpResponse &response) override;

private:
  struct CacheEntry {
    std::string body; // 原始文件内容（未解压）
    std::string content_type; // MIME 类型，例如 text/html
    std::string content_encoding; // 内容编码，例如 gzip / br；空串表示未压缩
    std::string last_modified; // 文件最后修改时间字符串（GMT 格式）
    std::string etag; // 资源 ETag（弱校验），用于 If-None-Match
    size_t content_length = 0;   // 原始 body 字节数
    Clock::time_point expires_at; // 缓存过期时间点
  };

  /**
   * @brief 判断请求路径是否应由当前中间件接管
   * @param path 请求路径（不含 query）
   * @return true 表示命中 `uri_prefix`，应走静态文件处理逻辑
   */
  bool should_handle_path(const std::string &path) const;

  /**
   * @brief 判断客户端是否声明支持指定内容编码
   * @param request HTTP 请求对象
   * @param encoding 编码名称，例如 `br` / `gzip`
   * @return true 表示 `Accept-Encoding` 中包含该编码
   */
  bool accepts_encoding(const HttpRequest::ptr &request,
                        const std::string &encoding) const;

private:
  Options options_;
  std::string normalized_prefix_; // 规范化后的 URI 前缀，便于快速匹配
  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::string, CacheEntry> cache_;
};

} // namespace zhttp

#endif // ZHTTP_STATIC_FILE_MIDDLEWARE_H_
