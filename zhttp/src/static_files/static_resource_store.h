#ifndef ZHTTP_STATIC_FILES_STATIC_RESOURCE_STORE_H_
#define ZHTTP_STATIC_FILES_STATIC_RESOURCE_STORE_H_
#include "zhttp/http_body.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace zhttp::detail {
struct StaticResource {
    std::string path, content_type, encoding, etag, last_modified;
    size_t size = 0;
    std::shared_ptr<const std::string> memory;
};
struct ResourceLookup {
    enum class State { Found, Missing, Forbidden, NotAcceptable } state = State::Missing;
    std::shared_ptr<const StaticResource> resource;
};
/** 文件选择、元信息、内容和有界缓存；不依赖请求、响应或中间件。 */
class StaticResourceStore {
  public:
    using Clock = std::chrono::steady_clock;
    struct CachePolicy {
        bool enabled = true;
        std::chrono::seconds ttl{5};
        size_t max_file_size = 1024 * 1024;
        size_t max_bytes = 16 * 1024 * 1024;
        size_t max_entries = 1024;
    };
    enum class ReadMode { Metadata, Content };
    StaticResourceStore(std::string root, std::string index, bool implicit_index,
                        CachePolicy cache, std::function<Clock::time_point()> now = Clock::now);
    ResourceLookup lookup(const std::string &relative, bool trailing_slash,
                          const std::vector<std::string> &encodings, ReadMode mode);
    static HttpBody body(const StaticResource &resource, size_t offset, size_t length);
    size_t cached_bytes() const;
    size_t cached_entries() const;
    void clear_expired();

  private:
    struct CacheEntry {
        std::shared_ptr<const StaticResource> resource;
        Clock::time_point expires;
    };
    void prune_locked(Clock::time_point now);
    void remember(std::shared_ptr<const StaticResource> resource);
    std::string root_, index_;
    bool implicit_index_;
    CachePolicy policy_;
    std::function<Clock::time_point()> now_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
    size_t cached_bytes_ = 0;
};
}
#endif
