#include "zhttp/parser/http_request_parser.h"
#include "znet/buffer.h"
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
bool HttpRequestParser::parse_line(const std::string &line) {
    auto first = line.find(' '), last = line.rfind(' ');
    if (first == std::string::npos || first == last ||
        line.find(' ', first + 1) != last)
        return false;
    const auto method = line.substr(0, first), version = line.substr(last + 1);
    request_->set_method(string_to_method(method));
    request_->set_version(string_to_version(version));
    if (request_->method() == HttpMethod::UNKNOWN ||
        method != method_to_string(request_->method()) ||
        request_->version() == HttpVersion::UNKNOWN)
        return false;
    try {
        request_->set_target(line.substr(first + 1, last - first - 1));
    } catch (...) {
        return false;
    }
    if ((request_->method() == HttpMethod::CONNECT) !=
        (request_->uri().form() == Uri::Form::Authority))
        return false;
    if (request_->uri().form() == Uri::Form::Asterisk &&
        request_->method() != HttpMethod::OPTIONS)
        return false;
    return true;
}
bool HttpRequestParser::finish_headers() {
    const auto lengths = request_->headers().get_all("Content-Length");
    const auto encodings = request_->headers().get_all("Transfer-Encoding");
    // 正文边界必须唯一，拒绝 Content-Length 与 Transfer-Encoding 同时存在。
    if (!encodings.empty() && !lengths.empty()) {
        fail(HttpStatus::BAD_REQUEST, "Content-Length with Transfer-Encoding");
        return false;
    }
    if (!encodings.empty()) {
        if (request_->version() != HttpVersion::HTTP_1_1 ||
            encodings.size() != 1 || to_lower(encodings.front()) != "chunked") {
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
    for (const auto &field : lengths)
        for (std::string text : split_string(field, ',')) {
            trim(text);
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
        }
    state_ = content_length_ ? ParseState::BODY : ParseState::COMPLETE;
    return true;
}
ParseResult HttpRequestParser::parse(znet::Buffer *buffer) {
    if (!buffer)
        return fail(HttpStatus::BAD_REQUEST, "Missing input buffer");
    while (true) {
        if (state_ == ParseState::ERROR)
            return ParseResult::ERROR;
        if (state_ == ParseState::COMPLETE)
            return ParseResult::COMPLETE;
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
        std::string line(buffer->peek(), crlf);
        buffer->retrieve(bytes);
        if (state_ == ParseState::REQUEST_LINE) {
            if (!parse_line(line))
                return fail(HttpStatus::BAD_REQUEST, "Invalid request line");
            state_ = ParseState::HEADERS;
            continue;
        }
        header_bytes_ += bytes;
        if (line.empty()) {
            if (!finish_headers())
                return ParseResult::ERROR;
            // 先报告头部就绪，调用方可在读正文前拒绝不支持的 Expect 等协议选项。
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
        trim(value);
        request_->append_header(name, value);
    }
}
} // namespace zhttp
