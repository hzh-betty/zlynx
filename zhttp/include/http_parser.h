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
 */
enum class ParseResult {
  OK,        // 解析成功，可继续
  COMPLETE,  // 完整请求解析完成
  NEED_MORE, // 需要更多数据
  ERROR      // 解析错误
};

/**
 * @brief HTTP 请求解析器
 * 支持流式解析，可处理不完整的请求数据
 */
class HttpParser {
public:
  HttpParser();

  /**
   * @brief 解析缓冲区中的数据
   * @param buffer 网络缓冲区
   * @return 解析结果
   */
  ParseResult parse(znet::Buffer *buffer);

  /**
   * @brief 获取解析完成的请求
   */
  HttpRequest::ptr request() const { return request_; }

  /**
   * @brief 重置解析器状态（用于 Keep-Alive 连接）
   */
  void reset();

  /**
   * @brief 获取当前解析状态
   */
  ParseState state() const { return state_; }

  /**
   * @brief 获取错误信息
   */
  const std::string &error() const { return error_; }

private:
  /**
   * @brief 解析请求行
   * @param begin 数据起始指针
   * @param end 数据结束指针
   * @return 解析结果
   */
  ParseResult parse_request_line(const char *begin, const char *end);

  /**
   * @brief 解析头部
   * @param begin 数据起始指针
   * @param end 数据结束指针
   * @return 解析结果
   */
  ParseResult parse_headers(const char *begin, const char *end);

  /**
   * @brief 解析请求体
   * @param buffer 网络缓冲区
   * @return 解析结果
   */
  ParseResult parse_body(znet::Buffer *buffer);

private:
  ParseState state_ = ParseState::REQUEST_LINE;
  HttpRequest::ptr request_;
  std::string error_;
  size_t content_length_ = 0;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_PARSER_H_
