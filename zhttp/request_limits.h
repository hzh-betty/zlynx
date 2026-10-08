#pragma once
#include <cstddef>
namespace zhttp {
struct RequestLimits {
    std::size_t max_request_line_bytes = 8 * 1024;
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_header_count = 100;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_chunk_line_bytes = 1024;
};
} // namespace zhttp
