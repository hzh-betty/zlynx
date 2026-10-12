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
/**
 * @brief 文件表示的元信息及可选内容快照；缓存条目不持有打开的文件描述符。
 *
 * memory 为空时仅有磁盘路径和元信息，之后打开文件不保证仍是同一版本。
 */
struct StaticResource {
    std::string path, content_type, encoding, etag, last_modified;
    size_t size = 0;
    std::shared_ptr<const std::string> memory;
};

struct ResourceLookup {
    enum class State {
        Found,
        Missing,
        Forbidden,
        NotAcceptable
    } state = State::Missing;
    std::shared_ptr<const StaticResource> resource;
};

/**
 * @brief 选择静态文件表示并缓存小文件内容，提供有界、带过期时间的快照。
 *
 * 缓存由 mutex_ 保护，文件检查和读取在锁外进行；未过期命中可返回旧快照。
 * 不执行路径净化或访问控制，调用方须先校验路径；注入时钟须支持并发调用。
 */
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
    StaticResourceStore(std::string root, std::string index,
                        bool implicit_index, CachePolicy cache,
                        std::function<Clock::time_point()> now = Clock::now);

    /**
     * @brief 按编码偏好选择首个可用文件表示，并视模式尝试加载小文件内容。
     *
     * @param relative 调用方已校验的相对路径，基于配置根目录拼接。
     * @param trailing_slash 是否带末尾斜杠，影响隐式首页选择。
     * @param encodings 按偏好排列的编码；空串表示原文件，br 表示 .br，gzip 表示
     * .gz。
     * @param mode Metadata 不主动读正文；缓存命中仍可能包含内容。Content
     * 按策略尝试读入小文件。
     * @return Found 携带共享只读资源；Missing 表示未找到或读取失败，Forbidden
     * 表示目录访问被禁用， NotAcceptable
     * 表示文件表示存在但没有符合编码偏好的候选。
     */
    ResourceLookup lookup(const std::string &relative, bool trailing_slash,
                          const std::vector<std::string> &encodings,
                          ReadMode mode);

    /**
     * @brief
     * 用指定字节范围创建独立正文；内存快照复制字节，磁盘资源重新打开路径。
     *
     * @param offset 起始字节偏移。
     * @param length 请求字节数；内存范围过长时截断至末尾，文件范围按
     * HttpBody::file 校验。
     * @throws std::out_of_range offset 超出内存快照长度。
     * @see HttpBody::file
     */
    static HttpBody body(const StaticResource &resource, size_t offset,
                         size_t length);
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

    // 仅统计缓存持有的内容字节，不计元信息或被外部共享者继续持有的已驱逐快照。
    size_t cached_bytes_ = 0;

    // 缓存到期时间的保守下界；允许偏早，以跳过无需扫描的查询。
    Clock::time_point earliest_expiry_ = Clock::time_point::max();
};
} // namespace zhttp::detail
#endif
