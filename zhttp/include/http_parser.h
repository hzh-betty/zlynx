#ifndef ZHTTP_HTTP_PARSER_H_
#define ZHTTP_HTTP_PARSER_H_

#include "http_request.h"

#include <string>

// 前向声明
namespace znet {
class Buffer;
}

namespace zhttp {

/**
 * @brief HTTP 解析状态
 *
 * 解析器按状态机方式工作：先读请求行，再读头部，最后按需读取 Body。
 * COMPLETE 和 ERROR 都是终止状态。
 */
enum class ParseState {
  REQUEST_LINE, // 解析请求行
  HEADERS,      // 解析头部
  BODY,         // 解析 Body
  COMPLETE,     // 解析完成
  ERROR         // 解析错误
};

/**
 * @brief HTTP 解析结果
 *
 * ParseState 描述“解析器现在处于什么阶段”，ParseResult 描述“这次调用 parse
 * 的返回结果是什么”。两者配合使用可以区分：是还要继续喂数据，还是已经完成，
 * 又或者当前请求本身不合法。
 */
enum class ParseResult {
  OK,        // 解析成功，可继续
  COMPLETE,  // 完整请求解析完成
  NEED_MORE, // 需要更多数据
  ERROR      // 解析错误
};

/**
 * @brief HTTP 请求解析器
 * @details
 * 解析器面向 TCP 流工作，不要求一次把完整请求全部读到内存。
 * 调用方可以在每次收到网络数据后调用 parse()，解析器会根据当前状态：
 * 1. 尝试消费已经到达的数据
 * 2. 数据不够时返回 NEED_MORE
 * 3. 完整请求到达后返回 COMPLETE
 *
 * 这种设计适合 Keep-Alive 和分片到达的场景。
 */
class HttpParser {
public:
  HttpParser();

  /**
   * @brief 解析缓冲区中的数据
   * @param buffer 网络缓冲区
   * @return 解析结果：可能是继续解析、等待更多数据、完成或出错
   */
  ParseResult parse(znet::Buffer *buffer);

  /**
   * @brief 获取解析完成的请求
   * @return 当前正在构建或已经构建完成的请求对象
   */
  HttpRequest::ptr request() const { return request_; }

  /**
   * @brief 重置解析器状态（用于 Keep-Alive 连接）
   * @details
   * 一个 TCP 连接上可能连续发送多个 HTTP 请求。处理完一个完整请求后，
   * 可以调用该函数恢复初始状态，以便继续解析下一个请求。
   */
  void reset();

  /**
   * @brief 获取当前解析状态
   * @return 当前状态机所处的阶段
   */
  ParseState state() const { return state_; }

  /**
   * @brief 获取错误信息
   * @return 最近一次解析失败的原因；成功路径下通常为空
   */
  const std::string &error() const { return error_; }

private:
  /**
   * @brief 解析请求行
   * @param begin 数据起始指针
   * @param end 数据结束指针
    * @return 解析结果
    * @details
    * 请求行格式固定为：METHOD SP URI SP VERSION。
    * 该函数会把 URI 拆成 path 和 query 两部分，并触发查询参数解析。
   */
  ParseResult parse_request_line(const char *begin, const char *end);

  /**
   * @brief 解析头部
   * @param begin 数据起始指针
   * @param end 数据结束指针
    * @return 解析结果
    * @details
    * 每次只解析一行头部。空行代表头部结束，此时解析器会根据 Content-Length
    * 判断是否还需要进入 Body 阶段。
   */
  ParseResult parse_headers(const char *begin, const char *end);

  /**
   * @brief 解析请求体
   * @param buffer 网络缓冲区
   * @return 解析结果
   * @details
   * 当前实现基于 Content-Length 读取定长 Body，不处理 chunked 编码。
   */
  ParseResult parse_body(znet::Buffer *buffer);

private:
  // 当前状态机阶段。
  ParseState state_ = ParseState::REQUEST_LINE;

  // 当前请求对象，解析过程中逐步填充字段。
  HttpRequest::ptr request_;

  // 最近一次错误的文字描述，便于日志和 400 响应输出。
  std::string error_;

  // 从 Content-Length 读取出的 Body 长度。
  size_t content_length_ = 0;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_PARSER_H_
