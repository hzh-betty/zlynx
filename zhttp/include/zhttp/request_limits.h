#ifndef ZHTTP_REQUEST_LIMITS_H_
#define ZHTTP_REQUEST_LIMITS_H_

#include <cstddef>
namespace zhttp {
/** HTTP 解析资源上限；长度单位为字节，包含 CRLF 的行按实际接收长度计数。 */
struct RequestLimits {
    /** 请求行最大长度。 */
    std::size_t max_request_line_bytes = 8 * 1024;
    /** 头部与 trailer 的累计最大长度。 */
    std::size_t max_header_bytes = 64 * 1024;
    /** 头部与 trailer 的累计最大字段数。 */
    std::size_t max_header_count = 100;
    /** 解码后正文最大长度。 */
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    /** chunk 长度行的最大长度。 */
    std::size_t max_chunk_line_bytes = 1024;
};
} // namespace zhttp

#endif // ZHTTP_REQUEST_LIMITS_H_
