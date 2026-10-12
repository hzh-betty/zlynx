#ifndef ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_
#define ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_

#include "zhttp/http_request.h"
#include "zhttp/request_limits.h"

namespace znet {
class ByteBuffer;
}

namespace zhttp {
/**
 * @brief 请求解析状态；完成和错误均保持到 reset()。
 */
enum class ParseState { REQUEST_LINE, HEADERS, BODY, COMPLETE, ERROR };

/**
 * @brief 解析结果；HEADERS_READY 允许调用方在读取正文前检查头部。
 */
enum class ParseResult { OK, HEADERS_READY, COMPLETE, NEED_MORE, ERROR };

/**
 * @brief 跨读取事件保存 chunked 解码进度，并累计正文与 trailer 上限。
 */
class ChunkedDecoder {
  public:
    /**
     * @brief 初始化 chunk 解码器，继承已有头部字节数和字段计数。
     */
    ChunkedDecoder(RequestLimits limits, size_t header_bytes,
                   size_t header_count)
        : limits_(limits), header_bytes_(header_bytes),
          header_count_(header_count) {}

    /**
     * @brief 增量消费 chunked 正文，保留跨读取事件的进度。
     *
     * @param buffer 已消费字节被移除，未完整的大小行或分隔符保留。
     * @param request 完成时接收正文；解析到的 trailer
     * 会立即追加，失败时不回滚。
     * @return NEED_MORE 等待输入，COMPLETE 表示全部正文及末尾空行完成，ERROR
     * 保持错误状态。
     */
    ParseResult parse(znet::ByteBuffer &buffer, HttpRequest &request);

    const std::string &error() const { return error_; }

    /**
     * @brief 返回错误对应的响应状态；仅 ERROR 时使用。
     */
    HttpStatus error_status() const { return error_status_; }

  private:
    enum class State { Size, Data, DataEnd, Trailers, Complete, Error };
    ParseResult fail(HttpStatus status, const char *message);
    RequestLimits limits_;

    // 头部预算含请求头和 trailer；remaining_ 是当前 chunk
    // 尚未收到的数据字节数。
    size_t header_bytes_, header_count_, remaining_ = 0;
    State state_ = State::Size;
    std::string body_, error_;
    HttpStatus error_status_ = HttpStatus::BAD_REQUEST;
};

/**
 * @brief 增量 HTTP/1.x 请求解析器。
 *
 * 每个连接独立持有；完成后保留请求，reset() 后才开始下一个请求。
 */
class HttpRequestParser {
  public:
    using Limits = RequestLimits;

    /**
     * @brief 使用指定资源上限构造并初始化解析器。
     */
    explicit HttpRequestParser(const RequestLimits &limits = {});

    /**
     * @brief 增量消费本次请求，先报告头部就绪，再解析正文。
     *
     * @param buffer 已消费字节被移除，后续请求字节保留；为空时进入 ERROR。
     * @return HEADERS_READY
     * 恰在本次头部完成时返回，包含无正文请求，须再次调用确认 COMPLETE；
     * NEED_MORE 等待更多输入，ERROR 查询错误信息。完成和错误保持到 reset()。
     */
    ParseResult parse(znet::ByteBuffer *buffer);

    /**
     * @brief 返回当前共享请求；仅 COMPLETE 时内容完整。
     */
    HttpRequest::ptr request() const { return request_; }

    /**
     * @brief 新建请求并清除解析状态；已有共享请求继续有效。
     */
    void reset();

    ParseState state() const { return state_; }

    const std::string &error() const { return error_; }

    /**
     * @brief 返回错误对应的响应状态；仅 ERROR 时使用。
     */
    HttpStatus error_status() const { return error_status_; }

  private:
    ParseResult fail(HttpStatus status, const char *message);
    bool parse_line(std::string_view line);
    bool finish_headers();
    RequestLimits limits_;
    HttpRequest::ptr request_;
    ParseState state_ = ParseState::REQUEST_LINE;
    std::string error_, body_;
    HttpStatus error_status_ = HttpStatus::BAD_REQUEST;

    // 请求行使用独立预算；头部字节含 CRLF 和最终空行，正文长度不计入头部预算。
    size_t header_bytes_ = 0, header_count_ = 0, content_length_ = 0;
    std::unique_ptr<ChunkedDecoder> chunked_;
};
} // namespace zhttp

#endif
