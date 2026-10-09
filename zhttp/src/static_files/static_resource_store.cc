#include "static_files/static_resource_store.h"
#include "static_files/path.h"
#include <fstream>
#include <sstream>
#include "zhttp/http_common.h"
#include <sys/stat.h>

namespace zhttp::detail {
namespace {
bool is_regular_file(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

bool is_directory(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool read_file(const std::string &path, std::string &content) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    content = oss.str();
    return true;
}

std::string detect_content_type(const std::string &file_path) {
    size_t dot = file_path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= file_path.size()) {
        return get_mime_type("");
    }
    return get_mime_type(file_path.substr(dot + 1));
}

std::string get_etag(const struct stat &st) {
    // 保留弱 ETag 格式：W/"size-mtime_ns"。
    uint64_t mtime_ns = static_cast<uint64_t>(st.st_mtime) * 1000000000ULL;
    mtime_ns += static_cast<uint64_t>(st.st_mtim.tv_nsec);

    std::ostringstream oss;
    oss << "W/\"" << static_cast<unsigned long long>(st.st_size) << "-"
        << static_cast<unsigned long long>(mtime_ns) << "\"";
    return oss.str();
}

}

StaticResourceStore::StaticResourceStore(std::string root, std::string index, bool implicit_index,
                                         CachePolicy cache, std::function<Clock::time_point()> now)
    : root_(std::move(root)), index_(std::move(index)), implicit_index_(implicit_index),
      policy_(cache), now_(std::move(now)) {
    if (!now_)
        now_ = Clock::now;
}
void StaticResourceStore::prune_locked(Clock::time_point now) {
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (now > it->second.expires) {
            cached_bytes_ -= it->second.resource->size;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}
void StaticResourceStore::clear_expired() {
    const auto now = now_();
    std::lock_guard<std::mutex> lock(mutex_);
    prune_locked(now);
}
size_t StaticResourceStore::cached_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_bytes_;
}
size_t StaticResourceStore::cached_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}
void StaticResourceStore::remember(std::shared_ptr<const StaticResource> resource) {
    if (!resource->memory || !policy_.enabled || policy_.ttl.count() <= 0 ||
        resource->size > policy_.max_file_size || resource->size > policy_.max_bytes ||
        policy_.max_entries == 0)
        return;
    const auto now = now_();
    std::lock_guard<std::mutex> lock(mutex_);
    prune_locked(now);
    auto existing = cache_.find(resource->path);
    if (existing != cache_.end()) {
        cached_bytes_ -= existing->second.resource->size;
        cache_.erase(existing);
    }
    while (!cache_.empty() && (cache_.size() >= policy_.max_entries ||
                               cached_bytes_ > policy_.max_bytes - resource->size)) {
        auto victim = cache_.begin();
        cached_bytes_ -= victim->second.resource->size;
        cache_.erase(victim);
    }
    cached_bytes_ += resource->size;
    const auto path = resource->path;
    cache_.emplace(path, CacheEntry{std::move(resource), now + policy_.ttl});
}
ResourceLookup StaticResourceStore::lookup(const std::string &relative, bool trailing_slash,
                                           const std::vector<std::string> &encodings, ReadMode mode) {
    clear_expired();
    std::string disk_path = static_path::join_path(root_, relative);
    if (is_directory(disk_path)) {
        if (!implicit_index_)
            return {ResourceLookup::State::Forbidden, {}};
        disk_path = static_path::join_path(disk_path, index_);
    } else if ((relative.empty() || trailing_slash) && implicit_index_) {
        disk_path = static_path::join_path(disk_path, index_);
    }
    for (const auto &encoding : encodings) {
        const std::string candidate = disk_path + (encoding.empty() ? "" : encoding == "br" ? ".br" : ".gz");
        if (policy_.enabled && policy_.ttl.count() > 0) {
            std::shared_ptr<const StaticResource> cached;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = cache_.find(candidate);
                if (it != cache_.end())
                    cached = it->second.resource;
            }
            if (cached)
                return {ResourceLookup::State::Found, std::move(cached)};
        }
        struct stat info{};
        if (::stat(candidate.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
            continue;
        auto resource = std::make_shared<StaticResource>();
        resource->path = candidate;
        resource->encoding = encoding;
        resource->content_type = detect_content_type(disk_path);
        resource->size = static_cast<size_t>(info.st_size);
        resource->etag = get_etag(info);
        resource->last_modified = format_http_date_gmt(info.st_mtime);
        if (mode == ReadMode::Content && policy_.enabled && resource->size <= policy_.max_file_size) {
            std::string bytes;
            if (!read_file(candidate, bytes))
                return {};
            resource->size = bytes.size();
            resource->memory = std::make_shared<const std::string>(std::move(bytes));
            remember(resource);
        }
        return {ResourceLookup::State::Found, std::move(resource)};
    }
    if (is_regular_file(disk_path) || is_regular_file(disk_path + ".br") ||
        is_regular_file(disk_path + ".gz"))
        return {ResourceLookup::State::NotAcceptable, {}};
    return {};
}
HttpBody StaticResourceStore::body(const StaticResource &resource, size_t offset, size_t length) {
    if (resource.memory)
        return HttpBody::memory(resource.memory->substr(offset, length));
    return HttpBody::file(resource.path, offset, length);
}
} // namespace zhttp::detail
