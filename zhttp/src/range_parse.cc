#include "zhttp/internal/range_parse.h"

#include "zhttp/http_common.h"

#include <algorithm>
#include <cctype>

namespace zhttp {

namespace {

bool parse_size_token(const std::string &token, size_t &value) {
    // 将字符串解析为 size_t。
    // 约束：
    // 1) 仅接受十进制无符号整数（不接受 +/-、空白、非数字）；
    // 2) 越界/格式异常统一返回 false，不向上抛异常。
    // 目的：让上层 Range 分支只关注“合法/非法”，不处理异常细节。
    if (token.empty()) {
        return false;
    }
    if (!std::all_of(token.begin(), token.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return false;
    }
    try {
        value = static_cast<size_t>(std::stoull(token));
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

void write_payload_by_range(const HttpRequest::ptr &request,
                            HttpResponse &response,
                            const ParsedRange &parsed_range,
                            size_t content_length, const std::string &content) {
    // 统一输出层：把 ParsedRange 映射到最终 HTTP 响应。
    // 分支优先级：416 -> 206 -> 200/HEAD 回落。

    // 416: 请求范围不可满足。
    // Content-Range 采用 "bytes */total"，让客户端知道当前实体总长。
    if (parsed_range.state == RangeParseState::NOT_SATISFIABLE) {
        response.status(HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE);
        response.header("Content-Range",
                        "bytes */" + std::to_string(content_length));
        response.body("");
        return;
    }

    // 206: 范围可满足，输出分片。
    // 注意 end 是 inclusive，因此长度是 end-start+1。
    if (parsed_range.state == RangeParseState::SATISFIABLE) {
        const size_t part_len = parsed_range.end - parsed_range.start + 1;
        response.status(HttpStatus::PARTIAL_CONTENT);
        response.header("Content-Range",
                        "bytes " + std::to_string(parsed_range.start) + "-" +
                            std::to_string(parsed_range.end) + "/" +
                            std::to_string(content_length));
        response.header("Content-Length", std::to_string(part_len));
        if (request->method() == HttpMethod::HEAD) {
            response.body("");
        } else {
            response.body(content.substr(parsed_range.start, part_len));
        }
        return;
    }

    // NONE / INVALID：按完整实体语义回落。
    // - HEAD: 仅回 Content-Length，不回 body；
    // - GET : 回完整 body。
    if (request->method() == HttpMethod::HEAD) {
        response.header("Content-Length", std::to_string(content_length));
        response.body("");
        return;
    }

    response.body(content);
}

ParsedRange parse_range_request(const HttpRequest::ptr &request,
                                size_t content_length,
                                const std::string &last_modified) {
    // 单范围解析器（MVP）：
    // - 支持 bytes=start-end / bytes=start- / bytes=-suffix；
    // - 不支持多范围（multipart/byteranges）；
    // - 与 If-Range 协作：If-Range 不匹配时主动回落 NONE。
    ParsedRange result;

    const std::string range_header = request->header("Range");
    // 没有 Range 头：交给调用方按整包处理（NONE）。
    if (range_header.empty()) {
        return result;
    }

    const std::string if_range = request->header("If-Range");
    if (!if_range.empty()) {
        // If-Range 不匹配时，RFC 语义为“忽略 Range，返回完整实体”。
        // 这里通过返回 NONE 表达“非错误的主动回落”。
        if (last_modified.empty() || if_range != last_modified) {
            return result;
        }
    }

    const std::string lowered = to_lower(range_header);
    // 仅处理 bytes 单位；其余单位按语法非法处理。
    if (lowered.size() < 7 || lowered.compare(0, 6, "bytes=") != 0) {
        result.state = RangeParseState::INVALID;
        return result;
    }

    std::string range_spec = range_header.substr(6);
    trim(range_spec);
    // 多范围（逗号）暂不支持，按 INVALID 处理；
    // 当前策略下 INVALID 会在输出层回落 200/HEAD。
    if (range_spec.empty() || range_spec.find(',') != std::string::npos) {
        result.state = RangeParseState::INVALID;
        return result;
    }

    const size_t dash = range_spec.find('-');
    if (dash == std::string::npos) {
        result.state = RangeParseState::INVALID;
        return result;
    }

    std::string first = range_spec.substr(0, dash);
    std::string second = range_spec.substr(dash + 1);
    trim(first);
    trim(second);

    if (first.empty() && second.empty()) {
        result.state = RangeParseState::INVALID;
        return result;
    }

    // 空文件无法满足任何有效字节范围。
    if (content_length == 0) {
        result.state = RangeParseState::NOT_SATISFIABLE;
        return result;
    }

    size_t start = 0;
    size_t end = 0;

    if (first.empty()) {
        // 后缀范围：bytes=-N，表示最后 N 字节。
        // N=0 或非数字都不可满足。
        size_t suffix_length = 0;
        if (!parse_size_token(second, suffix_length) || suffix_length == 0) {
            result.state = RangeParseState::NOT_SATISFIABLE;
            return result;
        }
        start =
            suffix_length < content_length ? content_length - suffix_length : 0;
        end = content_length - 1;
    } else {
        // 前缀/显式范围：bytes=start- 或 bytes=start-end。
        if (!parse_size_token(first, start)) {
            result.state = RangeParseState::INVALID;
            return result;
        }
        // 起点越界：不可满足（416）。
        if (start >= content_length) {
            result.state = RangeParseState::NOT_SATISFIABLE;
            return result;
        }

        if (second.empty()) {
            end = content_length - 1;
        } else {
            if (!parse_size_token(second, end)) {
                result.state = RangeParseState::INVALID;
                return result;
            }
            // 终点早于起点：语义不可满足。
            if (end < start) {
                result.state = RangeParseState::NOT_SATISFIABLE;
                return result;
            }
            // 允许客户端把 end 写到文件末尾之后，服务端截断到末尾。
            if (end >= content_length) {
                end = content_length - 1;
            }
        }
    }

    // 走到这里表示单范围可满足。
    result.state = RangeParseState::SATISFIABLE;
    result.start = start;
    result.end = end;
    return result;
}

} // namespace zhttp
