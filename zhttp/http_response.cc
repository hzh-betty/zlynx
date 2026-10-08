#include "zhttp/http_response.h"
#include <sstream>
#include <stdexcept>
namespace zhttp {
void HttpResponse::check_mutable() const {
    if (committed_)
        throw std::logic_error("Response already committed");
}
HttpResponse &HttpResponse::status(int code) {
    check_mutable();
    if (code < 100 || code > 599)
        throw std::invalid_argument("Invalid HTTP status");
    line_.status_code = static_cast<std::uint16_t>(code);
    return *this;
}
HttpResponse &HttpResponse::header(const std::string &key,
                                   const std::string &value) {
    check_mutable();
    headers_.set(key, value);
    return *this;
}
HttpResponse &HttpResponse::append_vary(const std::string &value) {
    std::string token = value;
    trim(token);
    if (token.empty())
        return *this;
    auto current = headers_.get("Vary");
    if (token == "*")
        return header("Vary", "*");
    if (header_contains_token(current, "*") ||
        header_contains_token(current, token))
        return *this;
    header("Vary", current.empty() ? token : current + ", " + token);
    return *this;
}
HttpResponse &HttpResponse::stream(StreamCallback callback,
                                   std::uint64_t length) {
    check_mutable();
    auto source = HttpBody::stream(std::move(callback), length);
    chunked_ = length == HttpBody::UnknownLength;
    return body(std::move(source));
}
std::string HttpResponse::build_set_cookie_value(const std::string &name,
                                                 const std::string &value,
                                                 const CookieOptions &opt) {
    // 按 Set-Cookie 语法把主值和可选属性顺序拼接起来。
    std::ostringstream oss;
    oss << name << "=" << value;

    if (!opt.path.empty()) {
        oss << "; Path=" << opt.path;
    }

    if (opt.max_age >= 0) {
        oss << "; Max-Age=" << opt.max_age;
    }

    if (opt.http_only) {
        oss << "; HttpOnly";
    }
    if (opt.secure) {
        oss << "; Secure";
    }

    if (!opt.same_site.empty()) {
        oss << "; SameSite=" << opt.same_site;
    }

    return oss.str();
}

HttpResponse &HttpResponse::set_cookie(const std::string &name,
                                       const std::string &value,
                                       const CookieOptions &opt) {
    // Set-Cookie 允许重复出现，所以不能简单塞进普通 headers_ 覆盖旧值。
    check_mutable();
    set_cookies_.push_back(build_set_cookie_value(name, value, opt));
    return *this;
}

HttpResponse &HttpResponse::delete_cookie(const std::string &name,
                                          const CookieOptions &opt) {
    // 通过 Max-Age=0 告诉浏览器立即删除该 Cookie。
    CookieOptions o = opt;
    o.max_age = 0;
    check_mutable();
    set_cookies_.push_back(build_set_cookie_value(name, "", o));
    return *this;
}

} // namespace zhttp
