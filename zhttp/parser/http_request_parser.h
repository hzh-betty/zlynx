#pragma once
#include "zhttp/http_request.h"
#include "zhttp/request_limits.h"
namespace znet {
class Buffer;
}
namespace zhttp {
enum class ParseState { REQUEST_LINE, HEADERS, BODY, COMPLETE, ERROR };
enum class ParseResult { OK, HEADERS_READY, COMPLETE, NEED_MORE, ERROR };

// 保存 chunk 长度、分片正文和 trailer 计数；跨读事件恢复属于解码器自身。
class ChunkedDecoder {
  public:
    ChunkedDecoder(RequestLimits limits, size_t header_bytes,
                   size_t header_count)
        : limits_(limits), header_bytes_(header_bytes),
          header_count_(header_count) {}
    ParseResult parse(znet::Buffer &buffer, HttpRequest &request);
    const std::string &error() const { return error_; }
    HttpStatus error_status() const { return error_status_; }

  private:
    enum class State { Size, Data, DataEnd, Trailers, Complete, Error };
    ParseResult fail(HttpStatus status, const char *message);
    RequestLimits limits_;
    size_t header_bytes_, header_count_, remaining_ = 0;
    State state_ = State::Size;
    std::string body_, error_;
    HttpStatus error_status_ = HttpStatus::BAD_REQUEST;
};

class HttpRequestParser {
  public:
    using Limits = RequestLimits;
    explicit HttpRequestParser(const RequestLimits &limits = {});
    ParseResult parse(znet::Buffer *buffer);
    HttpRequest::ptr request() const { return request_; }
    void reset();
    ParseState state() const { return state_; }
    const std::string &error() const { return error_; }
    HttpStatus error_status() const { return error_status_; }

  private:
    ParseResult fail(HttpStatus status, const char *message);
    bool parse_line(const std::string &line);
    bool finish_headers();
    RequestLimits limits_;
    HttpRequest::ptr request_;
    ParseState state_ = ParseState::REQUEST_LINE;
    std::string error_, body_;
    HttpStatus error_status_ = HttpStatus::BAD_REQUEST;
    size_t header_bytes_ = 0, header_count_ = 0, content_length_ = 0;
    std::unique_ptr<ChunkedDecoder> chunked_;
};
} // namespace zhttp
