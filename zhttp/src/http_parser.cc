#include "http_parser.h"

#include "zhttp_logger.h"
#include "znet/buffer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace zhttp {

// 构造时就准备一个空请求对象，后续解析过程中逐步填充字段。
HttpParser::HttpParser() : request_(std::make_shared<HttpRequest>()) {}

// 把解析器恢复到初始状态，便于在同一连接上继续解析下一条请求。
void HttpParser::reset() {
    state_ = ParseState::REQUEST_LINE;
    request_ = std::make_shared<HttpRequest>();
    error_.clear();
    content_length_ = 0;
    chunked_body_ = false;
    chunk_state_ = ChunkParseState::SIZE_LINE;
    chunked_body_buffer_.clear();
}

/**
 * parse() 是整个 HTTP 解析流程的入口。
 *
 * 这里按状态机循环推进：
 * 1. REQUEST_LINE / HEADERS 阶段按行读取，依赖 CRLF 定位一整行。
 * 2. BODY 阶段按 Content-Length 读取定长数据。
 * 3. 数据不足时立即返回 NEED_MORE，等待网络层继续投喂。
 */
ParseResult HttpParser::parse(znet::Buffer *buffer) {
    if (buffer == nullptr) {
        error_ = "Buffer is null";
        state_ = ParseState::ERROR;
        return ParseResult::ERROR;
    }

    ZHTTP_LOG_DEBUG("Parsing HTTP request, buffer size: {}",
                    buffer->readable_bytes());

    while (state_ != ParseState::COMPLETE && state_ != ParseState::ERROR) {
        if (buffer->readable_bytes() == 0) {
            return ParseResult::NEED_MORE;
        }

        if (state_ == ParseState::REQUEST_LINE ||
            state_ == ParseState::HEADERS) {
            // 请求行和头部都是逐行解析，所以先找一行结尾。
            const char *crlf = buffer->find_crlf();
            if (crlf == nullptr) {
                ZHTTP_LOG_DEBUG("Need more data, current state: {}",
                                state_ == ParseState::REQUEST_LINE
                                    ? "REQUEST_LINE"
                                    : "HEADERS");
                return ParseResult::NEED_MORE;
            }

            const char *begin = buffer->peek();
            const char *end = crlf;

            ParseResult result;
            if (state_ == ParseState::REQUEST_LINE) {
                ZHTTP_LOG_DEBUG("Parsing request line: {}",
                                std::string(begin, end));
                result = parse_request_line(begin, end);
            } else {
                ZHTTP_LOG_DEBUG("Parsing header: {}", std::string(begin, end));
                result = parse_headers(begin, end);
            }

            if (result == ParseResult::ERROR) {
                ZHTTP_LOG_ERROR("Parse error: {}", error_);
                return result;
            }

            // 当前这一行已经处理完，把它连同末尾的 CRLF 一起从缓冲区移走。
            buffer->retrieve(static_cast<size_t>(end - begin + 2));
        } else if (state_ == ParseState::BODY) {
            ZHTTP_LOG_DEBUG("Parsing body, expected length: {}, available: {}",
                            content_length_, buffer->readable_bytes());

            // Body 阶段不再按行处理，而是一次读取 Content-Length 指定的字节数。
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
    // 请求行格式固定为：METHOD SP REQUEST-URI SP HTTP-VERSION
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

    // 第二个空格前是 URI，里面可能同时包含 path 和 query。
    const char *space2 = std::find(space1 + 1, end, ' ');
    if (space2 == end) {
        error_ = "Invalid request line: no URI";
        state_ = ParseState::ERROR;
        return ParseResult::ERROR;
    }

    std::string uri(space1 + 1, space2);

    // path 和 query 分离后，请求对象后续访问参数会更方便。
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

    // 请求行成功后，下一阶段开始逐行解析头部。
    state_ = ParseState::HEADERS;
    return ParseResult::OK;
}

ParseResult HttpParser::parse_headers(const char *begin, const char *end) {
    // 空行表示头部结束，后面不是 Body 就是整个请求结束。
    if (begin == end) {
        // 按 RFC 语义：Transfer-Encoding: chunked 优先于 Content-Length。
        chunked_body_ = is_chunked_transfer_encoding();
        chunk_state_ = ChunkParseState::SIZE_LINE;
        chunked_body_buffer_.clear();

        if (chunked_body_) {
            state_ = ParseState::BODY;
            content_length_ = 0;
        } else {
            content_length_ = request_->content_length();
            if (content_length_ > 0) {
                state_ = ParseState::BODY;
            } else {
                state_ = ParseState::COMPLETE;
            }
        }
        return ParseResult::OK;
    }

    // 标准头部格式为 Key: Value。
    const char *colon = std::find(begin, end, ':');
    if (colon == end) {
        error_ = "Invalid header line: no colon";
        state_ = ParseState::ERROR;
        return ParseResult::ERROR;
    }

    std::string key(begin, colon);

    // 冒号后允许出现一个或多个空格，这里统一跳过。
    const char *value_start = colon + 1;
    while (value_start < end && *value_start == ' ') {
        ++value_start;
    }

    // 某些客户端会在值末尾带空格，这里顺手裁掉。
    const char *value_end = end;
    while (value_end > value_start && *(value_end - 1) == ' ') {
        --value_end;
    }

    std::string value(value_start, value_end);
    request_->set_header(key, value);

    return ParseResult::OK;
}

ParseResult HttpParser::parse_body(znet::Buffer *buffer) {
    if (chunked_body_) {
        return parse_chunked_body(buffer);
    }

    // Body 没收全之前不能继续向下走，避免拿到半包内容。
    if (buffer->readable_bytes() < content_length_) {
        return ParseResult::NEED_MORE;
    }

    // 一次性读取完整 Body，读取后缓冲区里的对应字节会被消费掉。
    std::string body = buffer->retrieve_as_string(content_length_);
    request_->set_body(std::move(body));

    state_ = ParseState::COMPLETE;
    return ParseResult::COMPLETE;
}

bool HttpParser::is_chunked_transfer_encoding() const {
    const std::string transfer_encoding = request_->header("Transfer-Encoding");
    if (transfer_encoding.empty()) {
        return false;
    }

    std::vector<std::string> tokens = split_string(transfer_encoding, ',');
    for (auto &token : tokens) {
        trim(token);
        if (to_lower(token) == "chunked") {
            return true;
        }
    }

    return false;
}

bool HttpParser::parse_chunk_size_line(const std::string &line,
                                       size_t *chunk_size) const {
    if (chunk_size == nullptr) {
        return false;
    }

    std::string chunk_line = line;
    trim(chunk_line);
    const size_t ext_pos = chunk_line.find(';');
    if (ext_pos != std::string::npos) {
        chunk_line = chunk_line.substr(0, ext_pos);
        trim(chunk_line);
    }

    if (chunk_line.empty()) {
        return false;
    }

    for (const char c : chunk_line) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    const unsigned long long parsed =
        std::strtoull(chunk_line.c_str(), nullptr, 16);
    if (parsed >
        static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return false;
    }

    *chunk_size = static_cast<size_t>(parsed);
    return true;
}

ParseResult HttpParser::parse_chunked_body(znet::Buffer *buffer) {
    // chunked 解析按如下循环推进：
    // SIZE_LINE -> DATA -> DATA_CRLF -> SIZE_LINE ... -> TRAILERS -> COMPLETE
    while (true) {
        if (buffer->readable_bytes() == 0) {
            return ParseResult::NEED_MORE;
        }

        if (chunk_state_ == ChunkParseState::SIZE_LINE) {
            // 读取当前块的 size 行，支持 "<hex>[;ext]" 形式。
            const char *crlf = buffer->find_crlf();
            if (crlf == nullptr) {
                return ParseResult::NEED_MORE;
            }

            const char *begin = buffer->peek();
            std::string line(begin, crlf);
            buffer->retrieve(static_cast<size_t>(crlf - begin + 2));

            size_t chunk_size = 0;
            if (!parse_chunk_size_line(line, &chunk_size)) {
                error_ = "Invalid chunk size";
                state_ = ParseState::ERROR;
                return ParseResult::ERROR;
            }

            // size=0 表示进入 trailer 区；否则读取该 chunk 的数据段。
            content_length_ = chunk_size;
            chunk_state_ = (chunk_size == 0) ? ChunkParseState::TRAILERS
                                             : ChunkParseState::DATA;
            continue;
        }

        if (chunk_state_ == ChunkParseState::DATA) {
            // 按 size 精确读取数据段，允许跨多个网络包拼齐。
            if (buffer->readable_bytes() < content_length_) {
                return ParseResult::NEED_MORE;
            }

            chunked_body_buffer_.append(buffer->peek(), content_length_);
            buffer->retrieve(content_length_);
            chunk_state_ = ChunkParseState::DATA_CRLF;
            continue;
        }

        if (chunk_state_ == ChunkParseState::DATA_CRLF) {
            // 每个数据段后必须紧跟 CRLF，缺失或格式错误都视为非法报文。
            if (buffer->readable_bytes() < 2) {
                return ParseResult::NEED_MORE;
            }

            const char *begin = buffer->peek();
            if (begin[0] != '\r' || begin[1] != '\n') {
                error_ = "Invalid chunk data terminator";
                state_ = ParseState::ERROR;
                return ParseResult::ERROR;
            }

            buffer->retrieve(2);
            chunk_state_ = ChunkParseState::SIZE_LINE;
            continue;
        }

        // 读取 trailer 区，直到遇到空行。trailer 字段当前只做语法校验并跳过。
        const char *crlf = buffer->find_crlf();
        if (crlf == nullptr) {
            return ParseResult::NEED_MORE;
        }

        const char *begin = buffer->peek();
        if (crlf == begin) {
            buffer->retrieve(2);
            request_->set_body(std::move(chunked_body_buffer_));
            state_ = ParseState::COMPLETE;
            return ParseResult::COMPLETE;
        }

        const char *colon = std::find(begin, crlf, ':');
        if (colon == crlf) {
            error_ = "Invalid chunk trailer";
            state_ = ParseState::ERROR;
            return ParseResult::ERROR;
        }

        buffer->retrieve(static_cast<size_t>(crlf - begin + 2));
    }
}

} // namespace zhttp
