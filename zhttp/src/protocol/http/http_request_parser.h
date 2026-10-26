#ifndef ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_
#define ZHTTP_PARSER_HTTP_REQUEST_PARSER_H_

#include "zhttp/http_request.h"
#include "zhttp/request_limits.h"

namespace znet {
class ByteBuffer;
}

namespace zhttp {
/*
 * ParseState 状态转换图（HTTP 请求解析进度）
 *
 *   创建 / reset()
 *          |
 *          v
 *   +--------------+   请求行解析成功   +---------+
 *   | REQUEST_LINE |------------------>| HEADERS |
 *   +--------------+                   +---------+
 *                                           |
 *                                    头部结束并校验通过
 *                                           |
 *                           +---------------+---------------+
 *                           |                               |
 *                 Content-Length > 0                   无正文
 *                    或使用 chunked                         |
 *                           v                               |
 *                       +------+                            |
 *                       | BODY |                            |
 *                       +------+                            |
 *                           |                               |
 *                       正文解析完成                         |
 *                           v                               v
 *                       +------------------------------------+
 *                       |              COMPLETE              |
 *                       +------------------------------------+
 *
 *   REQUEST_LINE / HEADERS / BODY --解析失败--> ERROR
 *   任意状态 -------------------reset()-----> REQUEST_LINE
 *
 * 状态说明：
 *   REQUEST_LINE : 等待或解析请求行
 *   HEADERS      : 等待或解析请求头
 *   BODY         : 按 Content-Length 或 chunked 解析正文
 *   COMPLETE     : 当前请求完整，保留请求对象供调用方使用
 *   ERROR        : 当前解析失败，保留错误信息和响应状态码
 *
 * 常规转换：
 *   创建解析器       -> REQUEST_LINE
 *   REQUEST_LINE     -> HEADERS      : 请求行合法
 *   HEADERS          -> BODY         : 存在非零定长正文或 chunked 正文
 *   HEADERS          -> COMPLETE     : 无正文或 Content-Length 为 0
 *   BODY             -> COMPLETE     : 正文完整
 *   非终态           -> ERROR        : 格式错误或资源上限校验失败
 *   任意状态         -> REQUEST_LINE : reset() 新建请求并清空解析进度
 *
 * 特殊路径：
 *   任意状态 -> ERROR : parse(nullptr)，包括原先已经 COMPLETE 的情况
 *   非终态   -> 原状态 : 输入不足，返回 NEED_MORE
 *   COMPLETE -> COMPLETE : 非空指针输入下重复 parse() 返回 COMPLETE
 *   ERROR    -> ERROR    : 重复 parse() 返回 ERROR
 *
 * 注意：
 *   HEADERS_READY 是返回结果，不是 ParseState。
 *   头部结束时先返回 HEADERS_READY，此时状态已经是 BODY 或 COMPLETE。
 *   即使无正文，也需要下一次 parse() 才向调用方报告 COMPLETE。
 *   解析完成后不会自动开始下一请求，须由调用方 reset()。
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
    /*
    * ChunkedDecoder::State 状态转换图（分块正文增量解码）
    *
    *   创建解码器
    *       |
    *       v
    *   +------+  chunk 大小 > 0  +------+  当前块数据收齐  +---------+
    *   | Size |----------------->| Data |---------------->| DataEnd |
    *   +------+                  +------+                 +---------+
    *       ^                                                   |
    *       +---------------- 数据后的 CRLF 合法 ----------------+
    *       |
    *       | chunk 大小 == 0
    *       v
    *   +----------+   trailer 末尾空行   +----------+
    *   | Trailers |-------------------->| Complete |
    *   +----------+                     +----------+
    *       |
    *       +-- 合法且非空的 trailer --> Trailers
    *
    *   Size / DataEnd / Trailers --校验失败--> Error
    *
    * 状态说明：
    *   Size     : 读取 chunk 大小行
    *   Data     : 读取当前 chunk 的 remaining_ 个数据字节
    *   DataEnd  : 检查当前 chunk 数据后的 CRLF
    *   Trailers : 读取零长度 chunk 后的 trailer 字段和最终空行
    *   Complete : 正文和 trailer 已完整接收
    *   Error    : 解码失败
    *
    * 常规转换：
    *   创建             -> Size
    *   Size             -> Data     : 合法的非零 chunk 大小
    *   Data             -> DataEnd  : 当前块数据收齐
    *   DataEnd          -> Size     : 数据后的 CRLF 合法
    *   Size             -> Trailers : chunk 大小为 0
    *   Trailers         -> Trailers : 追加一个合法 trailer
    *   Trailers         -> Complete : 遇到最终空行，向请求提交完整正文
    *
    * 失败路径：
    *   Size     -> Error : 大小行过长、大小非法/溢出、正文总量超限等
    *   DataEnd  -> Error : 数据后的分隔符不是 CRLF
    *   Trailers -> Error : 字节数/字段数超限、字段非法或使用禁止的字段名
    *
    * 注意：
    *   输入不足时保留当前状态，返回 NEED_MORE。
    *   Complete 和 Error 是终态，重复 parse() 返回相同结果。
    *   解码器没有 reset()；外层请求解析器复位时销毁它。
    *   零长度 chunk 不直接进入 Complete，仍须处理 trailer 和最终空行。
    *   trailer 逐项追加，失败时不回滚；正文到 Complete 才提交。
    */
    enum class State { Size, Data, DataEnd, Trailers, Complete, Error };
    ParseResult fail(HttpStatus status, const char *message);
    RequestLimits limits_;

    // 头部预算含请求头和 trailer；remaining_ 是当前 chunk
    // 尚未收到的数据字节数。
    size_t header_bytes_, header_count_, remaining_ = 0;
    State state_ = State::Size;
    std::string body_, error_; // 正文和错误信息
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
    
    /**
     * @brief 头部结束时检查 Content-Length、Transfer-Encoding 等字段，
     * 并设置正文解析状态；失败返回 false。
     *
     * @note 仅在 HEADERS 状态调用，成功后进入 BODY 或 COMPLETE。
     */
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
