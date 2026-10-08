#include "zhttp/parser/http_request_parser.h"
#include "znet/buffer.h"
#include <limits>
namespace zhttp {
ParseResult ChunkedDecoder::fail(HttpStatus status, const char *message) {
    error_status_ = status;
    error_ = message;
    state_ = State::Error;
    return ParseResult::ERROR;
}
ParseResult ChunkedDecoder::parse(znet::Buffer &buffer, HttpRequest &request) {
    while (true) {
        if (state_ == State::Error)
            return ParseResult::ERROR;
        if (state_ == State::Complete)
            return ParseResult::COMPLETE;
        // chunk 数据可跨多个读事件到达；remaining_ 保存尚缺字节数。
        if (state_ == State::Data) {
            auto count = std::min(remaining_, buffer.readable_bytes());
            if (count)
                body_.append(buffer.peek(), count);
            buffer.retrieve(count);
            remaining_ -= count;
            if (remaining_)
                return ParseResult::NEED_MORE;
            state_ = State::DataEnd;
        }
        if (state_ == State::DataEnd) {
            if (buffer.readable_bytes() < 2)
                return ParseResult::NEED_MORE;
            if (buffer.peek()[0] != '\r' || buffer.peek()[1] != '\n')
                return fail(HttpStatus::BAD_REQUEST,
                            "Invalid chunk data terminator");
            buffer.retrieve(2);
            state_ = State::Size;
        }
        if (!buffer.readable_bytes())
            return ParseResult::NEED_MORE;
        const auto *crlf = buffer.find_crlf();
        const size_t bytes = crlf
                                 ? static_cast<size_t>(crlf - buffer.peek()) + 2
                                 : buffer.readable_bytes();
        if (state_ == State::Size && bytes > limits_.max_chunk_line_bytes)
            return fail(HttpStatus::BAD_REQUEST, "Chunk size line too long");
        if (state_ == State::Trailers &&
            bytes > limits_.max_header_bytes - header_bytes_)
            return fail(HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE,
                        "Request trailers too large");
        if (!crlf)
            return ParseResult::NEED_MORE;
        std::string line(buffer.peek(), crlf);
        buffer.retrieve(bytes);
        if (state_ == State::Size) {
            if (!HttpHeaders::valid("chunk-size", line))
                return fail(HttpStatus::BAD_REQUEST, "Invalid chunk extension");
            auto semi = line.find(';');
            const auto digits = line.substr(0, semi);
            if (digits.empty())
                return fail(HttpStatus::BAD_REQUEST, "Invalid chunk size");
            size_t size = 0;
            for (unsigned char c : digits) {
                size_t d = c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : 16;
                if (d == 16 ||
                    size > (std::numeric_limits<size_t>::max() - d) / 16)
                    return fail(HttpStatus::BAD_REQUEST, "Invalid chunk size");
                size = size * 16 + d;
            }
            if (size > limits_.max_body_bytes - body_.size())
                return fail(HttpStatus::PAYLOAD_TOO_LARGE,
                            "Request body too large");
            remaining_ = size;
            // 零长度 chunk 只结束数据部分，仍须读取 trailer 和最终空行。
            state_ = size ? State::Data : State::Trailers;
        } else {
            // trailer 与请求头共享字节数和字段数预算，不能借末尾字段绕过上限。
            header_bytes_ += bytes;
            if (line.empty()) {
                request.set_body(std::move(body_));
                state_ = State::Complete;
                return ParseResult::COMPLETE;
            }
            if (++header_count_ > limits_.max_header_count)
                return fail(HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE,
                            "Too many request trailers");
            auto colon = line.find(':');
            if (colon == std::string::npos)
                return fail(HttpStatus::BAD_REQUEST, "Invalid chunk trailer");
            auto name = line.substr(0, colon), value = line.substr(colon + 1);
            // trailer 不能改变已经确定的正文边界、目标主机或连接语义。
            if (!HttpHeaders::valid(name, value) ||
                HttpHeaders::equal_name(name, "Content-Length") ||
                HttpHeaders::equal_name(name, "Transfer-Encoding") ||
                HttpHeaders::equal_name(name, "Host") ||
                HttpHeaders::equal_name(name, "Connection"))
                return fail(HttpStatus::BAD_REQUEST, "Invalid chunk trailer");
            trim(value);
            request.append_trailer(name, value);
        }
    }
}
} // namespace zhttp
