#include "http_parser.h"

#include "buff.h"
#include "zhttp_logger.h"

#include <algorithm>
#include <cstring>

namespace zhttp {

HttpParser::HttpParser() : request_(std::make_shared<HttpRequest>()) {}

void HttpParser::reset() {
  state_ = ParseState::REQUEST_LINE;
  request_ = std::make_shared<HttpRequest>();
  error_.clear();
  content_length_ = 0;
}

ParseResult HttpParser::parse(znet::Buffer *buffer) {
  ZHTTP_LOG_DEBUG("Parsing HTTP request, buffer size: {}",
                  buffer->readable_bytes());

  while (state_ != ParseState::COMPLETE && state_ != ParseState::ERROR) {
    if (state_ == ParseState::REQUEST_LINE || state_ == ParseState::HEADERS) {
      // 查找 CRLF
      const char *crlf = buffer->find_crlf();
      if (crlf == nullptr) {
        ZHTTP_LOG_DEBUG("Need more data, current state: {}",
                        state_ == ParseState::REQUEST_LINE ? "REQUEST_LINE"
                                                           : "HEADERS");
        return ParseResult::NEED_MORE;
      }

      const char *begin = buffer->peek();
      const char *end = crlf;

      ParseResult result;
      if (state_ == ParseState::REQUEST_LINE) {
        ZHTTP_LOG_DEBUG("Parsing request line: {}", std::string(begin, end));
        result = parse_request_line(begin, end);
      } else {
        ZHTTP_LOG_DEBUG("Parsing header: {}", std::string(begin, end));
        result = parse_headers(begin, end);
      }

      if (result == ParseResult::ERROR) {
        ZHTTP_LOG_ERROR("Parse error: {}", error_);
        return result;
      }

      // 消费已解析的数据（包括 CRLF）
      buffer->retrieve(static_cast<size_t>(end - begin + 2));
    } else if (state_ == ParseState::BODY) {
      ZHTTP_LOG_DEBUG("Parsing body, expected length: {}, available: {}",
                      content_length_, buffer->readable_bytes());
      ParseResult result = parse_body(buffer);
      if (result != ParseResult::COMPLETE) {
        return result;
      }
    }
  }

  if (state_ == ParseState::COMPLETE) {
    ZHTTP_LOG_INFO("HTTP request parsed successfully: {} {}",
                   method_to_string(request_->method()), request_->path());
    return ParseResult::COMPLETE;
  }
  return ParseResult::ERROR;
}

ParseResult HttpParser::parse_request_line(const char *begin, const char *end) {
  // 请求行格式: METHOD SP REQUEST-URI SP HTTP-VERSION
  const char *space1 = std::find(begin, end, ' ');
  if (space1 == end) {
    error_ = "Invalid request line: no method";
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
  }

  // 解析方法
  std::string method_str(begin, space1);
  HttpMethod method = string_to_method(method_str);
  if (method == HttpMethod::UNKNOWN) {
    error_ = "Unknown HTTP method: " + method_str;
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
  }
  request_->set_method(method);

  // 解析 URI
  const char *space2 = std::find(space1 + 1, end, ' ');
  if (space2 == end) {
    error_ = "Invalid request line: no URI";
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
  }

  std::string uri(space1 + 1, space2);

  // 分离路径和查询字符串
  size_t query_pos = uri.find('?');
  if (query_pos != std::string::npos) {
    request_->set_path(uri.substr(0, query_pos));
    request_->set_query(uri.substr(query_pos + 1));
    request_->parse_query_params();
  } else {
    request_->set_path(uri);
  }

  // 解析版本
  std::string version_str(space2 + 1, end);
  HttpVersion version = string_to_version(version_str);
  if (version == HttpVersion::UNKNOWN) {
    error_ = "Unknown HTTP version: " + version_str;
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
  }
  request_->set_version(version);

  state_ = ParseState::HEADERS;
  return ParseResult::OK;
}

ParseResult HttpParser::parse_headers(const char *begin, const char *end) {
  // 空行表示头部结束
  if (begin == end) {
    // 检查是否有 body
    content_length_ = request_->content_length();
    if (content_length_ > 0) {
      state_ = ParseState::BODY;
    } else {
      state_ = ParseState::COMPLETE;
    }
    return ParseResult::OK;
  }

  // 头部格式: Header-Name: Header-Value
  const char *colon = std::find(begin, end, ':');
  if (colon == end) {
    error_ = "Invalid header line: no colon";
    state_ = ParseState::ERROR;
    return ParseResult::ERROR;
  }

  std::string key(begin, colon);

  // 跳过冒号后的空格
  const char *value_start = colon + 1;
  while (value_start < end && *value_start == ' ') {
    ++value_start;
  }

  // 去除尾部空格
  const char *value_end = end;
  while (value_end > value_start && *(value_end - 1) == ' ') {
    --value_end;
  }

  std::string value(value_start, value_end);
  request_->set_header(key, value);

  return ParseResult::OK;
}

ParseResult HttpParser::parse_body(znet::Buffer *buffer) {
  if (buffer->readable_bytes() < content_length_) {
    return ParseResult::NEED_MORE;
  }

  std::string body = buffer->read_string(content_length_);
  request_->set_body(std::move(body));

  state_ = ParseState::COMPLETE;
  return ParseResult::COMPLETE;
}

} // namespace zhttp
