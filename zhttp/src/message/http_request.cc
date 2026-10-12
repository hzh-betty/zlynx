#include "zhttp/http_request.h"
#include <cstdlib>

namespace zhttp {
bool HttpRequest::is_keep_alive() const {
    bool keep_alive = false;
    for (const auto &field : headers_) {
        if (!HttpHeaders::equal_name(field.first, "Connection"))
            continue;
        if (header_contains_token(field.second, "close"))
            return false;
        keep_alive |= header_contains_token(field.second, "keep-alive");
    }
    return version() == HttpVersion::HTTP_1_1 || keep_alive;
}

std::size_t HttpRequest::content_length() const {
    return static_cast<std::size_t>(
        std::strtoull(header("Content-Length").c_str(), nullptr, 10));
}
} // namespace zhttp
