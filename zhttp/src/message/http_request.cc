#include "zhttp/http_request.h"
#include <cstdlib>
namespace zhttp {
bool HttpRequest::is_keep_alive() const {
    std::string connection;
    for (const auto &v : headers_.get_all("Connection")) {
        if (!connection.empty())
            connection += ",";
        connection += v;
    }
    if (header_contains_token(connection, "close"))
        return false;
    return version() == HttpVersion::HTTP_1_1 ||
           header_contains_token(connection, "keep-alive");
}
std::size_t HttpRequest::content_length() const {
    return static_cast<std::size_t>(
        std::strtoull(header("Content-Length").c_str(), nullptr, 10));
}
} // namespace zhttp
