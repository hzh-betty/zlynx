#include "zhttp/uri.h"
#include "zhttp/http_common.h"
#include <stdexcept>
namespace zhttp {
namespace {
bool hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
} // namespace
bool Uri::valid_encoding(const std::string &value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = value[i];
        if (c <= 32 || c == 127 || c == '#')
            return false;
        if (c == '%') {
            if (i + 2 >= value.size() || !hex(value[i + 1]) ||
                !hex(value[i + 2]))
                return false;
            i += 2;
        }
    }
    return true;
}
Uri::Uri(std::string target) : raw_(std::move(target)) {
    if (raw_.empty() || !valid_encoding(raw_))
        throw std::invalid_argument("Invalid request target");
    // 仅拆分请求目标，不解码路径；查询键值在访问时单独做百分号与 + 解码。
    auto q = raw_.find('?');
    path_ = raw_.substr(0, q);
    if (q != std::string::npos)
        query_ = raw_.substr(q + 1);
    if (raw_ == "*")
        form_ = Form::Asterisk;
    else if (raw_[0] == '/')
        form_ = Form::Origin;
    else if (raw_.find("://") != std::string::npos) {
        form_ = Form::Absolute;
        const auto scheme = path_.find("://");
        const auto authority_start = scheme + 3;
        const auto slash = path_.find('/', authority_start);
        if (scheme == 0 || authority_start == path_.size() ||
            slash == authority_start)
            throw std::invalid_argument("Invalid absolute request target");
        // 绝对形式请求目标只保留路由使用的路径，原始目标仍存放在 raw_。
        path_ = slash == std::string::npos ? "/" : path_.substr(slash);
    } else {
        form_ = Form::Authority;
        const auto colon = path_.rfind(':');
        if (q != std::string::npos || colon == std::string::npos ||
            colon == 0 || colon + 1 == path_.size() ||
            path_.find('/') != std::string::npos ||
            path_.find('@') != std::string::npos)
            throw std::invalid_argument("Invalid authority request target");
        for (size_t i = colon + 1; i < path_.size(); ++i)
            if (path_[i] < '0' || path_[i] > '9')
                throw std::invalid_argument("Invalid authority port");
    }
}
std::vector<std::string> Uri::query_values(const std::string &name) const {
    std::vector<std::string> values;
    for (const auto &pair : split_string(query_, '&')) {
        if (pair.empty())
            continue;
        auto eq = pair.find('=');
        if (url_decode(pair.substr(0, eq)) == name)
            values.push_back(
                eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1)));
    }
    return values;
}
std::string Uri::query_param(const std::string &name,
                             const std::string &fallback) const {
    auto values = query_values(name);
    return values.empty() ? fallback : values.front();
}
} // namespace zhttp
