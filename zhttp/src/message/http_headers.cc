#include "zhttp/http_headers.h"
#include <algorithm>
#include <stdexcept>
namespace zhttp {
namespace {
unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace
bool HttpHeaders::equal_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y) {
                          return lower(x) == lower(y);
                      });
}
bool HttpHeaders::valid(std::string_view name, std::string_view value) {
    if (name.empty())
        return false;
    for (unsigned char c : name) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              std::string_view("!#$%&'*+-.^_`|~").find(c) !=
                  std::string_view::npos))
            return false;
    }
    // 禁止 CR/LF 和其他控制字符，避免一个字段值扩展成额外协议行。
    for (unsigned char c : value)
        if ((c < 32 && c != '\t') || c == 127)
            return false;
    return true;
}
void HttpHeaders::append(std::string_view name, std::string_view value) {
    if (!valid(name, value))
        throw std::invalid_argument("Invalid HTTP field");
    // 输入可能借自现有字段，在 vector 扩容前保存拥有副本。
    fields_.emplace_back(std::string(name), std::string(value));
}
void HttpHeaders::set(std::string_view name, std::string_view value) {
    if (!valid(name, value))
        throw std::invalid_argument("Invalid HTTP field");
    // 校验通过后再删除旧字段，非法输入不会丢失原字段。
    Field field{std::string(name), std::string(value)};
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const Field &f) {
                                     return equal_name(f.first, field.first);
                                 }),
                  fields_.end());
    fields_.push_back(std::move(field));
}
HttpHeaders::const_iterator HttpHeaders::find(std::string_view name) const {
    return std::find_if(begin(), end(), [&](const Field &f) {
        return equal_name(f.first, name);
    });
}
std::string HttpHeaders::get(std::string_view name,
                             std::string_view fallback) const {
    auto it = find(name);
    return it == end() ? std::string(fallback) : it->second;
}
std::optional<std::string_view>
HttpHeaders::get_view(std::string_view name) const {
    auto it = find(name);
    if (it == end())
        return std::nullopt;
    return it->second;
}
std::size_t HttpHeaders::count(std::string_view name) const {
    return static_cast<std::size_t>(std::count_if(
        begin(), end(), [&](const Field &f) { return equal_name(f.first, name); }));
}
const std::string &HttpHeaders::at(std::string_view name) const {
    auto it = find(name);
    if (it == end())
        throw std::out_of_range(std::string(name));
    return it->second;
}
std::vector<std::string> HttpHeaders::get_all(std::string_view name) const {
    std::vector<std::string> result;
    for (const auto &f : fields_)
        if (equal_name(f.first, name))
            result.push_back(f.second);
    return result;
}
void HttpHeaders::erase(std::string_view name) {
    // name 可能指向即将被删除或移动的字段。
    const std::string owned_name(name);
    fields_.erase(std::remove_if(fields_.begin(), fields_.end(),
                                 [&](const Field &f) {
                                     return equal_name(f.first, owned_name);
                                 }),
                  fields_.end());
}
} // namespace zhttp
