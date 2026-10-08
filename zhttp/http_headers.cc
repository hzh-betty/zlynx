#include "zhttp/http_headers.h"
#include <algorithm>
#include <stdexcept>
namespace zhttp {
namespace {
unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace
bool HttpHeaders::equal_name(const std::string &a, const std::string &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y) {
                          return lower(x) == lower(y);
                      });
}
bool HttpHeaders::valid(const std::string &name, const std::string &value) {
    if (name.empty())
        return false;
    for (unsigned char c : name) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos))
            return false;
    }
    for (unsigned char c : value)
        if ((c < 32 && c != '\t') || c == 127)
            return false;
    return true;
}
void HttpHeaders::append(const std::string &name, const std::string &value) {
    if (!valid(name, value))
        throw std::invalid_argument("Invalid HTTP field");
    fields_.emplace_back(name, value);
}
void HttpHeaders::set(const std::string &name, const std::string &value) {
    if (!valid(name, value))
        throw std::invalid_argument("Invalid HTTP field");
    erase(name);
    append(name, value);
}
HttpHeaders::const_iterator HttpHeaders::find(const std::string &name) const {
    return std::find_if(begin(), end(), [&](const Field &f) {
        return equal_name(f.first, name);
    });
}
std::string HttpHeaders::get(const std::string &name,
                             const std::string &fallback) const {
    auto it = find(name);
    return it == end() ? fallback : it->second;
}
const std::string &HttpHeaders::at(const std::string &name) const {
    auto it = find(name);
    if (it == end())
        throw std::out_of_range(name);
    return it->second;
}
std::vector<std::string> HttpHeaders::get_all(const std::string &name) const {
    std::vector<std::string> result;
    for (const auto &f : fields_)
        if (equal_name(f.first, name))
            result.push_back(f.second);
    return result;
}
void HttpHeaders::erase(const std::string &name) {
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const Field &f) {
                                     return equal_name(f.first, name);
                                 }),
                  fields_.end());
}
} // namespace zhttp
