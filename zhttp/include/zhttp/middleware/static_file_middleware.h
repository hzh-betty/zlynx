/**
 * static_file_middleware.h
 * static_file_middleware 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHHTP_STATIC_OPT_H_
#define ZHHTP_STATIC_OPT_H_

#include "zhttp/middleware/middleware.h"
#include <chrono>

#include <string>

namespace zhttp {
namespace detail { class StaticResourceStore; }
namespace mid {
/**
 * 静态文件分发中间件
 *
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
     * 静态文件中间件配置项
     */
    struct Options {
        Options()
            : uri_prefix("/"), document_root("."), index_file("index.html"),
              cache_control("public, max-age=60"), enable_implicit_index(true),
              enable_last_modified(true), enable_etag(true),
              enable_memory_cache(true), gzip_static(true), br_static(true),
              memory_cache_time(5), max_cached_file_size(1024 * 1024) {}

        std::string uri_prefix;      // 需要拦截的 URL 前缀，例如 /assets
        std::string document_root;   // 静态资源根目录
        std::string index_file;      // 目录请求默认文件名，例如 index.html
        std::string cache_control;   // 回写到响应中的 Cache-Control
        bool enable_implicit_index;  // 是否允许目录自动补 index 文件
        bool enable_last_modified;   // 是否启用 Last-Modified / 304 逻辑
        bool enable_etag;            // 是否启用 ETag / If-None-Match 逻辑
        bool enable_memory_cache;    // 是否启用内存缓存
        bool gzip_static;            // 是否启用 .gz 预压缩文件分发
        bool br_static;              // 是否启用 .br 预压缩文件分发
        int memory_cache_time;       // 内存缓存 TTL（秒）
        size_t max_cache_bytes = 16 * 1024 * 1024; // 缓存总字节上限
        size_t max_cache_entries = 1024; // 缓存条目上限
        size_t max_cached_file_size; // 允许进入内存缓存的最大文件体积（字节）
    };

    /**
     * 使用默认配置构造静态文件中间件
     */
    StaticFileMiddleware();

    /**
     * 使用自定义配置构造静态文件中间件
     *
     * @param options 中间件配置
     */
    explicit StaticFileMiddleware(Options options);

    /**
     * 请求前置处理
     * 未命中静态文件时返回 true，命中时构造文件响应并返回 false
     */
    bool before(HttpContext &context) override;
    ~StaticFileMiddleware() override;

  private:
    /**
     * 判断请求路径是否应由当前中间件接管
     *
     * @param path 请求路径（不含 query）
     * @return true 表示命中 `uri_prefix`，应走静态文件处理逻辑
     */
    bool should_handle_path(const std::string &path) const;

  private:
    Options options_;
    std::string normalized_prefix_; // 规范化后的 URI 前缀，便于快速匹配
    std::unique_ptr<detail::StaticResourceStore> store_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHHTP_STATIC_OPT_H_
