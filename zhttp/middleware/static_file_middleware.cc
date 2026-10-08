/**
 * static_file_middleware.cc
 * static_file_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/static_file_middleware.h"

#include <vector>

#include "zhttp/http_common.h"
#include "zhttp/internal/http_utils.h"
#include "zhttp/internal/range_parse.h"

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
      normalized_prefix_(static_path::normalize_prefix(options_.uri_prefix)) {}

StaticFileMiddleware::StaticFileMiddleware()
    : StaticFileMiddleware(Options()) {}

bool StaticFileMiddleware::before(HttpContext &request) {
    auto &response = request.response();
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
    const std::string &path = request.path();
    if (!should_handle_path(path)) {
        // 返回 true = 本中间件“不消费请求”，交给后续中间件/路由继续处理。
        return true;
    }

    // 静态资源只支持 GET/HEAD。
    if (request.method() != HttpMethod::GET &&
        request.method() != HttpMethod::HEAD) {
        // 返回 false = 已经生成响应并终止后续路由。
        response.status(HttpStatus::METHOD_NOT_ALLOWED)
            .header("Allow", "GET, HEAD")
            .text("Method Not Allowed");
        return false;
    }

    // 阶段 2：URI -> 相对路径，并进行路径穿越防护。
    std::string relative_raw =
        static_path::map_to_relative_path(path, normalized_prefix_);
    std::string relative;
    if (!static_path::sanitize_relative_path(relative_raw, relative)) {
        // 任何可疑路径（如 ..）都直接 403，避免访问 document_root 外部文件。
        response.status(HttpStatus::FORBIDDEN).text("Forbidden");
        return false;
    }

    // 阶段 3：拼接磁盘路径，并处理目录 index 回落。
    bool path_ends_with_slash =
        !relative_raw.empty() && relative_raw.back() == '/';
    std::string disk_path =
        static_path::join_path(options_.document_root, relative);

    if (file::is_directory(disk_path)) {
        // 命中真实目录：按配置决定是否自动补 index 文件。
        if (!options_.enable_implicit_index) {
            response.status(HttpStatus::FORBIDDEN).text("Forbidden");
            return false;
        }
        disk_path = static_path::join_path(disk_path, options_.index_file);
    } else if ((relative.empty() || path_ends_with_slash) &&
               options_.enable_implicit_index) {
        // 目标不是目录，但路径语义上像“目录入口”（空路径或尾 '/'），也尝试
        // index 回落。
        disk_path = static_path::join_path(disk_path, options_.index_file);
    }

    // 阶段 4：编码协商（仅做“候选顺序”决策，真正选文件在后面）。
    const auto encoding_candidates =
        accepted_content_encodings(request.header("Accept-Encoding"),
                                   options_.br_static, options_.gzip_static);

    const std::string content_type =
        file::detect_content_type(disk_path);

    // 按协商优先级逐个检查缓存和文件，低权重缓存不能抢在高权重文件之前。
    std::string content_encoding;
    std::string selected_path;
    struct stat selected_info = {};
    for (const auto &enc : encoding_candidates) {
        if (options_.enable_memory_cache && options_.memory_cache_time > 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = cache_.find(path + "|" + enc);
            if (it != cache_.end()) {
                if (std::chrono::steady_clock::now() <= it->second.expires_at) {
                    if (try_handle_conditional_not_modified(
                            request, response, options_, it->second.etag,
                            it->second.last_modified,
                            it->second.content_encoding)) {
                        return false;
                    }
                    response.status(HttpStatus::OK);
                    apply_entity_headers(
                        response, options_, it->second.content_type,
                        it->second.content_encoding, it->second.etag,
                        it->second.last_modified);
                    const ParsedRange parsed_range =
                        parse_range_request(request, it->second.content_length,
                                            it->second.last_modified);
                    write_payload_by_range(request, response, parsed_range,
                                           it->second.content_length,
                                           it->second.body);
                    response.set_keep_alive(request.is_keep_alive());
                    return false;
                }
                cache_.erase(it);
            }
        }
        const std::string candidate = disk_path + (enc.empty()   ? ""
                                                   : enc == "br" ? ".br"
                                                                 : ".gz");
        if (::stat(candidate.c_str(), &selected_info) == 0 &&
            S_ISREG(selected_info.st_mode)) {
            selected_path = candidate;
            content_encoding = enc;
            break;
        }
    }

    if (selected_path.empty()) {
        if (file::is_regular_file(disk_path) ||
            file::is_regular_file(disk_path + ".br") ||
            file::is_regular_file(disk_path + ".gz")) {
            response.status(HttpStatus::NOT_ACCEPTABLE).body("");
            response.header("Vary", "Accept-Encoding");
            return false;
        }
        // 中间件不强制 404，交给后续业务路由（可用于 SPA fallback 等场景）。
        return true;
    }

    // 阶段 7：磁盘回源命中后，处理 Last-Modified 条件请求。
    std::string last_modified;
    std::string etag;
    if (options_.enable_etag) {
        etag = file::get_etag(selected_info);
    }

    if (options_.enable_last_modified) {
        last_modified = format_http_date_gmt(selected_info.st_mtime);
    }

    if (try_handle_conditional_not_modified(request, response, options_, etag,
                                            last_modified, content_encoding)) {
        return false;
    }

    if (!options_.enable_memory_cache ||
        static_cast<uint64_t>(selected_info.st_size) >
            options_.max_cached_file_size ||
        request.method() == HttpMethod::HEAD) {
        response.status(HttpStatus::OK);
        apply_entity_headers(response, options_, content_type, content_encoding,
                             etag, last_modified);
        const auto total = static_cast<size_t>(selected_info.st_size);
        const auto range = parse_range_request(request, total, last_modified);
        if (range.state == RangeParseState::NOT_SATISFIABLE) {
            response.status(HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE)
                .header("Content-Range", "bytes */" + std::to_string(total))
                .body("");
        } else {
            const auto offset =
                range.state == RangeParseState::SATISFIABLE ? range.start : 0;
            const auto length = range.state == RangeParseState::SATISFIABLE
                                    ? range.end - range.start + 1
                                    : total;
            try {
                response.body(HttpBody::file(selected_path, offset, length));
            } catch (...) {
                return true;
            }
            response.header("Content-Length", std::to_string(length));
            if (range.state == RangeParseState::SATISFIABLE)
                response.status(HttpStatus::PARTIAL_CONTENT)
                    .header("Content-Range",
                            "bytes " + std::to_string(range.start) + "-" +
                                std::to_string(range.end) + "/" +
                                std::to_string(total));
        }
        response.set_keep_alive(request.is_keep_alive());
        return false;
    }

    // 阶段 8：读取文件并组装响应（HEAD 不回包体，只回 Content-Length）。
    std::string content;
    if (!file::read_file(selected_path, content)) {
        // 文件在 stat/read 之间可能被删除或不可读，回退给后续路由统一处理。
        return true;
    }

    response.status(HttpStatus::OK);
    apply_entity_headers(response, options_, content_type, content_encoding,
                         etag, last_modified);

    const ParsedRange parsed_range =
        parse_range_request(request, content.size(), last_modified);

    write_payload_by_range(request, response, parsed_range, content.size(),
                           content);
    response.set_keep_alive(request.is_keep_alive());

    // 阶段 9：响应成功后按配置写入内存缓存（只缓存小文件）。
    if (options_.enable_memory_cache && options_.memory_cache_time > 0 &&
        content.size() <= options_.max_cached_file_size) {
        // 缓存体与响应体一致；HEAD 请求也会缓存，便于后续 GET 复用。
        CacheEntry entry;
        entry.body = std::move(content);
        entry.content_type = content_type;
        entry.content_encoding = content_encoding;
        entry.last_modified = last_modified;
        entry.etag = etag;
        entry.content_length = entry.body.size();
        entry.expires_at = std::chrono::steady_clock::now() +
                           std::chrono::seconds(options_.memory_cache_time);
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[path + "|" + content_encoding] = std::move(entry);
    }

    return false;
}

bool StaticFileMiddleware::should_handle_path(const std::string &path) const {
    return static_path::should_handle_path(path, normalized_prefix_);
}

} // namespace mid
} // namespace zhttp
