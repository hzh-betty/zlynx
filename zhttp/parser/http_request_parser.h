#ifndef ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_
#define ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_

#include "zhttp/http_request.h"
#include "zhttp/request_limits.h"
namespace znet {
class ByteBuffer;
}
namespace zhttp {
/** 请求解析状态；完成和错误均保持到 reset()。 */
enum class ParseState { REQUEST_LINE, HEADERS, BODY, COMPLETE, ERROR };
/** 解析结果；HEADERS_READY 允许调用方在读取正文前检查头部。 */
enum class ParseResult { OK, HEADERS_READY, COMPLETE, NEED_MORE, ERROR };

// 保存 chunk 长度、分片正文和 trailer 计数；跨读事件恢复属于解码器自身。
/** 跨读取事件保存 chunked 解码进度，并累计正文与 trailer 上限。 */
class ChunkedDecoder {
  public:
    /** 初始化 chunk 解码器，继承已有头部字节数和字段计数。 */
    ChunkedDecoder(RequestLimits limits, size_t header_bytes,
                   size_t header_count)
        : limits_(limits), header_bytes_(header_bytes),
          header_count_(header_count) {}
    /** 消费完整 chunk 部分；未完整部分保留进度，失败可查询 error()。 */
    ParseResult parse(znet::ByteBuffer &buffer, HttpRequest &request);
    /** 返回最近解析错误文本。 */
    const std::string &error() const { return error_; }
    /** 返回错误对应的响应状态；仅 ERROR 时使用。 */
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

/**
 * 增量 HTTP/1.x 请求解析器。
 *
 * 每个连接独立持有；完成后保留请求，reset() 后才开始下一个请求。
 */
class HttpRequestParser {
  public:
    using Limits = RequestLimits;
    /** 使用指定资源上限构造并初始化解析器。 */
    explicit HttpRequestParser(const RequestLimits &limits = {});
    /**
     * 消费输入缓冲区中的本次请求数据。
     *
     * @param buffer 可读输入；已消费字节被移除，后续请求字节保留。
     * @return HEADERS_READY 后需再次调用；NEED_MORE 等待更多输入，ERROR 查询错误信息。
     */
    ParseResult parse(znet::ByteBuffer *buffer);
    /** 返回当前共享请求；仅 COMPLETE 时内容完整。 */
    HttpRequest::ptr request() const { return request_; }
    /** 新建请求并清除解析状态；已有共享请求继续有效。 */
    void reset();
    /** 返回当前解析状态。 */
    ParseState state() const { return state_; }
    /** 返回最近解析错误文本。 */
    const std::string &error() const { return error_; }
    /** 返回错误对应的响应状态；仅 ERROR 时使用。 */
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

#endif // ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_
