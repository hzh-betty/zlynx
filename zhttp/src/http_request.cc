/**
 * @file http_request.cc
 * @brief http_request 实现。
 * @author hzh-betty
 */

#include "zhttp/http_request.h"

#include "zhttp/http_common.h"
#include "zhttp/multipart.h"

#include <cstdlib>

namespace zhttp {

namespace {

/**
 * 解析 Cookie 请求头。
 *
 * 典型输入："a=1; theme=dark; flag"
 * 解析结果：
 * - a -> 1
 * - theme -> dark
 * - flag -> ""
 *
 * 注意这里解析的是请求头里的 Cookie，而不是响应头里的 Set-Cookie，
 * 两者语义和格式并不完全一样。
 */
static void parse_cookie_header(const std::string &cookie_header,
                                HttpRequest::Params &out) {
    // 每次解析前清空输出，避免重复调用时残留旧数据。
    out.clear();
    size_t pos = 0;
    while (pos < cookie_header.size()) {
        // 一个 cookie 项通常由分号分隔。
        size_t end = cookie_header.find(';', pos);
        if (end == std::string::npos) {
            end = cookie_header.size();
        }

        std::string token = cookie_header.substr(pos, end - pos);
        trim(token);
        if (!token.empty()) {
            // key=value 是最常见格式；某些场景下也可能只有 key。
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                std::string key = token.substr(0, eq);
                std::string value = token.substr(eq + 1);
                trim(key);
                trim(value);
                if (!key.empty()) {
                    out[key] = value;
                }
            } else {
                // 没有等号时仍然保留这个标记，值置空。
                out[token] = "";
            }
        }

        // 跳到下一个 token 的起始位置。
        pos = end + 1;
    }
}
} // namespace

/**
 * 大小写不敏感地读取请求头。
 *
 * 虽然 headers_ 当前按原始 key 保存，但 HTTP 头字段名本身不区分大小写，
 * 因此通过归一化索引做一次查表，保证 Header、header、HEADER 都能命中。
 */
std::string HttpRequest::header(const std::string &key,
                                const std::string &default_val) const {
    auto it = normalized_headers_.find(to_lower(key));
    if (it != normalized_headers_.end()) {
        return it->second;
    }
    return default_val;
}

// 路由匹配阶段提取出的路径参数，按 key 直接查询即可。
std::string HttpRequest::path_param(const std::string &key,
                                    const std::string &default_val) const {
    auto it = path_params_.find(key);
    if (it != path_params_.end()) {
        return it->second;
    }
    return default_val;
}

// 查询参数已经在 parse_query_params 中标准化，这里只做简单查表。
std::string HttpRequest::query_param(const std::string &key,
                                     const std::string &default_val) const {
    auto it = query_params_.find(key);
    if (it != query_params_.end()) {
        return it->second;
    }
    return default_val;
}

/**
 * 按需解析 Cookie。
 *
 * 不是所有请求都会访问 Cookie，因此这里采用延迟解析：
 * 只有第一次调用 cookie()/cookies() 时才真正解析请求头。
 * 这样可以减少不必要的字符串处理开销。
 */
void HttpRequest::parse_cookies_if_needed() {
    if (runtime_.cookies_parsed) {
        return;
    }

    // 先置位，保证即使头不存在也不会重复进入解析流程。
    runtime_.cookies_parsed = true;
    std::string cookie_header = header("Cookie");
    if (cookie_header.empty()) {
        runtime_.cookies.clear();
        return;
    }

    parse_cookie_header(cookie_header, runtime_.cookies);
}

// 读取单个 Cookie，未命中时返回调用方给定的默认值。
std::string HttpRequest::cookie(const std::string &key,
                                const std::string &default_val) const {
    const_cast<HttpRequest *>(this)->parse_cookies_if_needed();
    auto it = runtime_.cookies.find(key);
    if (it != runtime_.cookies.end()) {
        return it->second;
    }
    return default_val;
}

// 返回全部 Cookie；这里同样依赖延迟解析。
const HttpRequest::Params &HttpRequest::cookies() const {
    const_cast<HttpRequest *>(this)->parse_cookies_if_needed();
    return runtime_.cookies;
}

const std::string &HttpRequest::remote_addr() const { return remote_addr_; }

void HttpRequest::set_remote_addr(const std::string &addr) {
    remote_addr_ = addr;
}

void HttpRequest::set_remote_addr(std::string &&addr) {
    remote_addr_ = std::move(addr);
}

// 头字段按原始 key 保存，同时维护一份归一化索引加速大小写不敏感查找。
void HttpRequest::set_header(const std::string &key, const std::string &value) {
    const std::string normalized_key = to_lower(key);
    headers_[key] = value;
    normalized_headers_[normalized_key] = value;

    // 影响请求体解析语义的头变化后，清理缓存，避免旧结果污染。
    if (normalized_key == "content-type") {
        body_.invalidate();
    }
}

// 路径参数通常由路由器写入，这里只是简单落盘。
void HttpRequest::set_path_param(const std::string &key,
                                 const std::string &value) {
    path_params_[key] = value;
}

/**
 * 解析 URL 中 ? 后面的查询字符串。
 *
 * 例如：name=betty&debug=true&empty
 * 解析后得到：
 * - name -> betty
 * - debug -> true
 * - empty -> ""
 */
void HttpRequest::parse_query_params() {
    // 重新解析前先清空，避免请求对象复用时保留旧值。
    query_params_.clear();
    detail::parse_urlencoded_params(query_, query_params_);
}

/**
 * 判断连接是否应保持长连接。
 *
 * 规则来自 HTTP 协议版本约定：
 * - HTTP/1.1 默认长连接，除非显式写 Connection: close
 * - HTTP/1.0 默认短连接，只有写 Connection: keep-alive 才保持
 */
bool HttpRequest::is_keep_alive() const {
    std::string connection = header("Connection");
    if (version_ == HttpVersion::HTTP_1_1) {
        // HTTP/1.1 默认为 keep-alive
        return to_lower(connection) != "close";
    }
    // HTTP/1.0 默认关闭
    return to_lower(connection) == "keep-alive";
}

// 从请求头读取 Content-Length；缺失时按 0 处理。
size_t HttpRequest::content_length() const {
    std::string len_str = header("Content-Length");
    if (len_str.empty()) {
        return 0;
    }
    // 这里使用 strtoul，非法内容会退化为 0，调用方应结合协议校验整体合法性。
    return static_cast<size_t>(std::strtoul(len_str.c_str(), nullptr, 10));
}

// 直接返回原始 Content-Type 字段，便于上层自己决定是否进一步解析参数。
std::string HttpRequest::content_type() const { return header("Content-Type"); }

// 仅判断主 MIME 是否为 application/json，忽略 charset 等附加参数。
bool HttpRequest::is_json() const {
    return detail::RequestBody::is_json(content_type());
}

/**
 * 解析 JSON 请求体。
 *
 * 语义约定：
 * - 非 JSON 请求直接返回 true（表示“无需解析”，不是错误）；
 * - 空 JSON Body 视为错误，返回 false 并记录 json_error_；
 * - 非法 JSON 文本返回 false 并记录 json_error_；
 * - 成功时缓存结果，后续重复调用不重复解析。
 */
bool HttpRequest::parse_json() { return body_.parse_json(content_type()); }

// const 场景下也允许触发惰性解析，因此通过 const_cast 复用实现。
const HttpRequest::Json *HttpRequest::json() const {
    return body_.json(content_type());
}

// 判断是否为 URL 编码表单请求体。
bool HttpRequest::is_form_urlencoded() const {
    return detail::RequestBody::is_form_urlencoded(content_type());
}

/**
 * 解析 application/x-www-form-urlencoded 请求体。
 *
 * 语义约定：
 * - 非该类型请求返回 true（无需解析）；
 * - 空 body 返回 true（结果为空表）；
 * - 同名 key 采用“后写覆盖前写”；
 * - 无等号片段记为空字符串，例如 "flag" -> flag=""。
 */
bool HttpRequest::parse_form_urlencoded() {
    return body_.parse_form_urlencoded(content_type());
}

// 惰性读取表单字段，首次访问时自动触发解析。
const HttpRequest::Params &HttpRequest::form_params() const {
    return body_.form_params(content_type());
}

// 按 key 读取表单字段，未命中返回默认值。
std::string HttpRequest::form_param(const std::string &key,
                                    const std::string &default_val) const {
    const auto &fields = form_params();
    auto it = fields.find(key);
    if (it != fields.end()) {
        return it->second;
    }
    return default_val;
}

// multipart/form-data 常用于文件上传，这里只做快速识别，不负责真正解析。
bool HttpRequest::is_multipart() const {
    return detail::RequestBody::is_multipart(content_type());
}

/**
 * 解析 multipart/form-data 请求体。
 *
 * 这里同样采用延迟解析并带缓存：
 * - 如果已经解析过，直接复用上次结果
 * - 如果当前请求不是 multipart，视为无需解析，返回 true
 * - 真正的边界解析逻辑交给 MultipartFormData::parse
 */
bool HttpRequest::parse_multipart() {
    return body_.parse_multipart(content_type(), [this](std::string *error) {
        return MultipartFormData::parse(*this, error);
    });
}

// const 接口也允许触发懒解析，因此这里通过 const_cast 复用已有实现。
const MultipartFormData *HttpRequest::multipart() const {
    return body_.multipart(content_type(), [this](std::string *error) {
        return MultipartFormData::parse(*this, error);
    });
}

} // namespace zhttp
