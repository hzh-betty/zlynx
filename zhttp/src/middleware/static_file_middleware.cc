/**
 * static_file_middleware.cc
 * static_file_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/static_file_middleware.h"

#include <vector>

#include "zhttp/http_common.h"
#include "static_files/path.h"
#include "static_files/range.h"
#include "static_files/static_resource_store.h"

namespace zhttp {
namespace mid {

namespace {

std::string normalize_etag_token(std::string token) {
    trim(token);
    std::string lowered = to_lower(token);
    if (lowered.size() > 2 && lowered[0] == 'w' && lowered[1] == '/') {
        return token.substr(2);
    }
    return token;
}

bool if_none_match_matches(const std::string &if_none_match,
                           const std::string &etag) {
    // If-None-Match 匹配语义（用于 GET/HEAD 条件缓存协商）：
    // 1) 支持 "*" 通配符：只要资源存在即视为匹配；
    // 2) 支持逗号分隔多 ETag；
    // 3) 采用“弱比较”策略：比较时忽略 W/ 前缀（W/"x" 与 "x" 视为可匹配）；
    // 4) 仅做 token 层匹配，不解析复杂引号转义（当前 ETag
    // 生成策略无需该复杂度）。
    //
    // 说明：这里用于 If-None-Match，按 RFC 可使用弱比较；If-Match
    // 则通常需要强比较。
    if (if_none_match.empty() || etag.empty()) {
        return false;
    }

    const std::string normalized_etag = normalize_etag_token(etag);
    size_t begin = 0;
    while (begin < if_none_match.size()) {
        size_t end = if_none_match.find(',', begin);
        if (end == std::string::npos) {
            end = if_none_match.size();
        }
        std::string token = if_none_match.substr(begin, end - begin);
        trim(token);

        if (token == "*") {
            return true;
        }

        if (!token.empty() && normalize_etag_token(token) == normalized_etag) {
            return true;
        }

        begin = end + 1;
    }

    return false;
}

// 负责“内容协商相关”响应头：
// - Cache-Control：缓存策略；
// - Vary: Accept-Encoding：告知缓存层响应受编码协商影响；
// - Content-Encoding：标识实体编码（gzip/br/identity）。
// 该函数不处理实体校验头（ETag/Last-Modified）。
void apply_variant_headers(HttpResponse &response,
                           const StaticFileMiddleware::Options &options,
                           const std::string &content_encoding) {
    if (!options.cache_control.empty()) {
        response.header("Cache-Control", options.cache_control);
    }
    if (options.gzip_static || options.br_static) {
        response.append_vary("Accept-Encoding");
    }
    if (!content_encoding.empty()) {
        response.header("Content-Encoding", content_encoding);
    }
}

// 负责“资源校验器”响应头：
// - ETag：更精细的实体版本标识；
// - Last-Modified：兼容传统时间戳校验。
// 调用方可按配置传空字符串，函数会自动跳过对应头部。
void apply_validator_headers(HttpResponse &response, const std::string &etag,
                             const std::string &last_modified) {
    if (!etag.empty()) {
        response.header("ETag", etag);
    }
    if (!last_modified.empty()) {
        response.header("Last-Modified", last_modified);
    }
}

// 负责“实体响应通用头”聚合：
// - Content-Type；
// - Accept-Ranges（声明支持字节范围请求）；
// - 内容协商头与校验器头。
// 该函数不负责状态码、响应体以及条件请求判定。
void apply_entity_headers(HttpResponse &response,
                          const StaticFileMiddleware::Options &options,
                          const std::string &content_type,
                          const std::string &content_encoding,
                          const std::string &etag,
                          const std::string &last_modified) {
    response.content_type(content_type);
    response.header("Accept-Ranges", "bytes");
    apply_variant_headers(response, options, content_encoding);
    apply_validator_headers(response, etag, last_modified);
}

// 条件请求短路处理：命中则直接写 304 并返回 true。
// 规则：
// 1) If-None-Match 优先；
// 2) 仅当未携带 If-None-Match 时才评估 If-Modified-Since；
// 3) 命中时统一补齐协商/校验相关响应头并保持 keep-alive 语义。
//
// 返回值：
// - true  : 已完成 304 响应，调用方应立即结束流程；
// - false : 条件未命中，调用方继续构造正常实体响应。
bool try_handle_conditional_not_modified(
    HttpContext &request, HttpResponse &response,
    const StaticFileMiddleware::Options &options, const std::string &etag,
    const std::string &last_modified, const std::string &content_encoding) {
    // 按 RFC 优先级处理：If-None-Match 优先于 If-Modified-Since。
    if (options.enable_etag) {
        const std::string inm = request.header("If-None-Match");
        if (!inm.empty() && if_none_match_matches(inm, etag)) {
            response.status(HttpStatus::NOT_MODIFIED);
            apply_variant_headers(response, options, content_encoding);
            apply_validator_headers(response, etag, last_modified);
            response.set_keep_alive(request.is_keep_alive());
            return true;
        }
    }

    if (options.enable_last_modified &&
        request.header("If-None-Match").empty()) {
        const std::string ims = request.header("If-Modified-Since");
        if (!ims.empty() && ims == last_modified) {
            response.status(HttpStatus::NOT_MODIFIED);
            apply_variant_headers(response, options, content_encoding);
            apply_validator_headers(response, etag, last_modified);
            response.set_keep_alive(request.is_keep_alive());
            return true;
        }
    }

    return false;
}

} // namespace

StaticFileMiddleware::StaticFileMiddleware(Options options)
    : options_(std::move(options)),
      normalized_prefix_(static_path::normalize_prefix(options_.uri_prefix)),
      store_(std::make_unique<detail::StaticResourceStore>(
          options_.document_root, options_.index_file, options_.enable_implicit_index,
          detail::StaticResourceStore::CachePolicy{
              options_.enable_memory_cache, std::chrono::seconds{options_.memory_cache_time},
              options_.max_cached_file_size, options_.max_cache_bytes, options_.max_cache_entries})) {}
StaticFileMiddleware::StaticFileMiddleware() : StaticFileMiddleware(Options()) {}
StaticFileMiddleware::~StaticFileMiddleware() = default;

bool StaticFileMiddleware::before(HttpContext &request) {
    const auto &path = request.path();
    if (!should_handle_path(path))
        return true;
    auto &response = request.response();
    if (request.method() != HttpMethod::GET && request.method() != HttpMethod::HEAD) {
        response.status(HttpStatus::METHOD_NOT_ALLOWED).header("Allow", "GET, HEAD").text("Method Not Allowed");
        return false;
    }
    const auto raw = static_path::map_to_relative_path(path, normalized_prefix_);
    std::string relative;
    if (!static_path::sanitize_relative_path(raw, relative)) {
        response.status(HttpStatus::FORBIDDEN).text("Forbidden");
        return false;
    }
    auto selected = store_->lookup(relative, !raw.empty() && raw.back() == '/',
        accepted_content_encodings(request.header("Accept-Encoding"), options_.br_static, options_.gzip_static),
        request.method() == HttpMethod::HEAD ? detail::StaticResourceStore::ReadMode::Metadata
                                            : detail::StaticResourceStore::ReadMode::Content);
    using State = detail::ResourceLookup::State;
    if (selected.state == State::Missing)
        return true;
    if (selected.state == State::Forbidden) {
        response.status(HttpStatus::FORBIDDEN).text("Forbidden");
        return false;
    }
    if (selected.state == State::NotAcceptable) {
        response.status(HttpStatus::NOT_ACCEPTABLE).body("").header("Vary", "Accept-Encoding");
        return false;
    }
    const auto &resource = *selected.resource;
    const auto etag = options_.enable_etag ? resource.etag : "";
    const auto modified = options_.enable_last_modified ? resource.last_modified : "";
    if (try_handle_conditional_not_modified(request, response, options_, etag, modified, resource.encoding))
        return false;
    const auto range = parse_range_request(request, resource.size, modified);
    HttpBody body;
    size_t offset = 0, length = resource.size;
    if (range.state == RangeParseState::SATISFIABLE) {
        offset = range.start;
        length = range.end - range.start + 1;
    }
    if (range.state != RangeParseState::NOT_SATISFIABLE) {
        try {
            body = detail::StaticResourceStore::body(resource, offset, length);
        } catch (...) {
            // 文件打开失败时回退路由；在成功打开前不留下静态响应的头部。
            return true;
        }
    }
    response.status(HttpStatus::OK);
    apply_entity_headers(response, options_, resource.content_type, resource.encoding, etag, modified);
    if (range.state == RangeParseState::NOT_SATISFIABLE) {
        response.status(HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE)
            .header("Content-Range", "bytes */" + std::to_string(resource.size)).body("");
    } else {
        response.body(std::move(body)).header("Content-Length", std::to_string(length));
        if (range.state == RangeParseState::SATISFIABLE)
            response.status(HttpStatus::PARTIAL_CONTENT).header("Content-Range",
                "bytes " + std::to_string(range.start) + "-" + std::to_string(range.end) + "/" + std::to_string(resource.size));
    }
    response.set_keep_alive(request.is_keep_alive());
    return false;
}

bool StaticFileMiddleware::should_handle_path(const std::string &path) const {
    return static_path::should_handle_path(path, normalized_prefix_);
}
} // namespace mid
} // namespace zhttp
