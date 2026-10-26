#include "protocol/http/http_request_parser.h"
#include "znet/byte_buffer.h"
#include <algorithm>

namespace zhttp {
HttpRequestParser::HttpRequestParser(const RequestLimits &limits)
    : limits_(limits) {
    reset();
}

void HttpRequestParser::reset() {
    request_ = std::make_shared<HttpRequest>();
    state_ = ParseState::REQUEST_LINE;
    error_.clear();
    body_.clear();
    error_status_ = HttpStatus::BAD_REQUEST;
    header_bytes_ = header_count_ = content_length_ = 0;
    chunked_.reset();
}

ParseResult HttpRequestParser::fail(HttpStatus status, const char *message) {
    error_status_ = status;
    error_ = message;
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
}
// METHOD SP TARGET SP VERSION CRLF
bool HttpRequestParser::parse_line(std::string_view line) {
    // 请求行必须包含两个空格，且 METHOD 和 VERSION 必须合法。
    auto first = line.find(kSP), last = line.rfind(kSP);
    if (first == std::string::npos || first == last ||
        line.find(kSP, first + 1) != last)
        return false;
    const auto method = line.substr(0, first), version = line.substr(last + 1);
    request_->set_method(string_to_method(method));
    request_->set_version(string_to_version(version));
    
    if (request_->method() == HttpMethod::UNKNOWN ||
        method != method_to_string(request_->method()) ||
        request_->version() == HttpVersion::UNKNOWN)
        return false;
    try {
        request_->set_target(
            std::string(line.substr(first + 1, last - first - 1)));
    } catch (...) {
        return false;
    }

    // 请求行的 METHOD 与 TARGET 组合必须合法；CONNECT 仅允许 authority，OPTIONS * 仅允许星号。
    if ((request_->method() == HttpMethod::CONNECT) !=
        (request_->uri().form() == Uri::Form::Authority))
        return false;
    if (request_->uri().form() == Uri::Form::Asterisk &&
        request_->method() != HttpMethod::OPTIONS)
        return false;
    return true;
}

bool HttpRequestParser::finish_headers() {
    const auto &headers = request_->headers();
    const auto encodings = headers.count("Transfer-Encoding");

    // 正文边界必须唯一，拒绝 Content-Length 与 Transfer-Encoding 同时存在。
    if (encodings && headers.count("Content-Length")) {
        fail(HttpStatus::BAD_REQUEST, "Content-Length with Transfer-Encoding");
        return false;
    }

    // 仅支持 HTTP/1.1 chunked 正文；其他 Transfer-Encoding 或 HTTP/1.0 chunked 都拒绝。
    if (encodings) {
        if (request_->version() != HttpVersion::HTTP_1_1 || encodings != 1 ||
            !HttpHeaders::equal_name(request_->header_view("Transfer-Encoding"),
                                     "chunked")) {
            fail(HttpStatus::BAD_REQUEST, "Unsupported Transfer-Encoding");
            return false;
        }
        chunked_.reset(
            new ChunkedDecoder(limits_, header_bytes_, header_count_));
        state_ = ParseState::BODY;
        return true;
    }

    // 重复或逗号合并的 Content-Length 必须完全一致；逐位累积时先检查正文上限。
    bool first = true;
    for (const auto &field : headers) {
        if (!HttpHeaders::equal_name(field.first, "Content-Length"))
            continue;
        std::string_view remaining = field.second;
        while (true) {
            const auto comma = remaining.find(',');
            const auto text = trim_view(remaining.substr(0, comma));
            if (text.empty()) {
                fail(HttpStatus::BAD_REQUEST, "Invalid Content-Length");
                return false;
            }
            size_t length = 0;
            for (char c : text) {
                if (c < '0' || c > '9') {
                    fail(HttpStatus::BAD_REQUEST, "Invalid Content-Length");
                    return false;
                }
                size_t d = c - '0';
                if (d > limits_.max_body_bytes ||
                    length > (limits_.max_body_bytes - d) / 10) {
                    fail(HttpStatus::PAYLOAD_TOO_LARGE,
                         "Request body too large");
                    return false;
                }
                length = length * 10 + d;
            }
            if (!first && content_length_ != length) {
                fail(HttpStatus::BAD_REQUEST, "Conflicting Content-Length");
                return false;
            }
            content_length_ = length;
            first = false;
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }
    }
    state_ = content_length_ ? ParseState::BODY : ParseState::COMPLETE;
    return true;
}

ParseResult HttpRequestParser::parse(znet::ByteBuffer *buffer) {
    if (!buffer)
        return fail(HttpStatus::BAD_REQUEST, "Missing input buffer");
    while (true) {
        // 已失败 → 返回 ERROR
        if (state_ == ParseState::ERROR)
            return ParseResult::ERROR;
        // 已完成 → 返回 COMPLETE
        if (state_ == ParseState::COMPLETE)
            return ParseResult::COMPLETE;

        // 处理正文；chunked 正文可跨多个读事件到达，非 chunked 正文只消费当前请求所需字节。
        if (state_ == ParseState::BODY) {
            if (chunked_) {
                const auto result = chunked_->parse(*buffer, *request_);
                if (result == ParseResult::ERROR) {
                    error_ = chunked_->error();
                    error_status_ = chunked_->error_status();
                    state_ = ParseState::ERROR;
                }
                if (result == ParseResult::COMPLETE)
                    state_ = ParseState::COMPLETE;
                return result;
            }

            // 只消费当前正文尚缺的字节，将流水线中的下一个请求留在输入缓冲区。
            const auto count = std::min(content_length_ - body_.size(),
                                        buffer->readable_bytes());
            if (count)
                body_.append(buffer->peek(), count);
            buffer->retrieve(count);
            if (body_.size() < content_length_)
                return ParseResult::NEED_MORE;
            request_->set_body(std::move(body_));
            state_ = ParseState::COMPLETE;
            return ParseResult::COMPLETE;
        }
        if (!buffer->readable_bytes())
            return ParseResult::NEED_MORE;

        // 解析请求行或头部；半行不消费，保留在输入缓冲区。
        const auto *crlf = buffer->find_crlf();
        const size_t bytes =
            crlf ? static_cast<size_t>(crlf - buffer->peek()) + 2
                 : buffer->readable_bytes();
        if (state_ == ParseState::REQUEST_LINE &&
            bytes > limits_.max_request_line_bytes)
            return fail(HttpStatus::URI_TOO_LONG, "Request line too long");
        if (state_ == ParseState::HEADERS &&
            bytes > limits_.max_header_bytes - header_bytes_)
            return fail(HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE,
                        "Request headers too large");

        // 半行不消费，保留在 Buffer；字节限制已在上方检查，避免无限等待换行。
        if (!crlf)
            return ParseResult::NEED_MORE;

        const std::string_view line(buffer->peek(), bytes - 2);
        buffer->retrieve(bytes);

        // 处理请求行；成功后进入头部，失败返回 ERROR。
        if (state_ == ParseState::REQUEST_LINE) {
            if (!parse_line(line))
                return fail(HttpStatus::BAD_REQUEST, "Invalid request line");
            state_ = ParseState::HEADERS;
            continue;
        }

        header_bytes_ += bytes;

        // 如果头部行为空，则表示头部结束，进入正文或完成状态。
        if (line.empty()) {
            if (!finish_headers())
                return ParseResult::ERROR;

            // 先报告头部就绪，调用方可在读正文前拒绝不支持的 Expect
            // 等协议选项。
            return ParseResult::HEADERS_READY;
        }
        if (++header_count_ > limits_.max_header_count)
            return fail(HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE,
                        "Too many request headers");
        auto colon = line.find(':');
        if (colon == std::string::npos)
            return fail(HttpStatus::BAD_REQUEST, "Invalid header line");
        auto name = line.substr(0, colon), value = line.substr(colon + 1);
        if (!HttpHeaders::valid(name, value))
            return fail(HttpStatus::BAD_REQUEST, "Invalid header field");
        value = trim_view(value);
        request_->append_header(name, value);
    }
}
} // namespace zhttp
