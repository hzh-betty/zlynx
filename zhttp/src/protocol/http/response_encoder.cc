#include "protocol/http/response_encoder.h"
#include <charconv>
#include <limits>
#include <stdexcept>

namespace zhttp {

std::string ResponseEncoder::chunk(const char *data, std::size_t size) {
    if (!size)
        return "";

    // 每四个值位最多占一个十六进制字符；to_chars 不写末尾 NUL。
    char line[(std::numeric_limits<std::size_t>::digits + 3) / 4];
    const auto result = std::to_chars(line, line + sizeof(line), size, 16);
    std::string frame(line, result.ptr);
    frame.append(kCRLF);
    frame.append(data, size);
    frame.append(kCRLF);
    return frame;
}

ResponsePlan ResponseEncoder::plan(const HttpRequestLine &request,
                                   const HttpResponse &response, bool upgrade) {
    const auto code = response.status_line().status_code;
    if (code < 100 || code > 599 ||
        (response.version() != HttpVersion::HTTP_1_0 &&
         response.version() != HttpVersion::HTTP_1_1))
        throw std::invalid_argument("Invalid response status line");
    if (code < 200 && !(upgrade && code == 101))
        throw std::invalid_argument("Informational response cannot be final");
    ResponsePlan p;
    const auto &body = response.body_source();

    // HEAD 仍可携带实体长度，但不发送正文；无正文状态码同样跳过正文。
    p.send_body = request.method != HttpMethod::HEAD &&
                  is_body_allowed(response.status_code());
    p.length = body.length_known() ? body.length() : HttpBody::UnknownLength;
    p.chunked =
        p.send_body && response.version() == HttpVersion::HTTP_1_1 &&
        (response.is_chunked_enabled() || p.length == HttpBody::UnknownLength);

    // HTTP/1.0 无法对未知长度使用 chunked，必须以关闭连接界定正文末尾。
    p.close =
        !response.is_keep_alive() ||
        (p.send_body && !p.chunked && p.length == HttpBody::UnknownLength);
    for (const auto &field : response.headers())
        if (HttpHeaders::equal_name(field.first, "Connection") &&
            header_contains_token(field.second, "close"))
            p.close = true;
    return p;
}

std::string ResponseEncoder::headers(const HttpRequestLine &request,
                                     const HttpResponse &response,
                                     bool upgrade) {
    return headers(response, plan(request, response, upgrade), upgrade);
}

std::string ResponseEncoder::headers(const HttpResponse &response,
                                     const ResponsePlan &p, bool upgrade) {
    const auto code = response.status_line().status_code;
    std::string out;
    size_t capacity = 128;
    for (const auto &h : response.headers())
        capacity += h.first.size() + h.second.size() + 4;
    for (const auto &cookie : response.set_cookies())
        capacity += cookie.size() + 13;
    out.reserve(capacity);
    out.append(version_to_string(response.version()));
    out.push_back(kSP);
    out.append(std::to_string(code));
    out.push_back(kSP);
    out.append(status_to_string(response.status_code()));
    out.append(kCRLF);

    // 正文边界和连接头由写出计划统一生成，忽略业务层设置的同名字段。
    for (const auto &h : response.headers()) {
        if (HttpHeaders::equal_name(h.first, "Content-Length") ||
            HttpHeaders::equal_name(h.first, "Transfer-Encoding") ||
            HttpHeaders::equal_name(h.first, "Connection"))
            continue;
        out.append(h.first).append(": ").append(h.second).append(kCRLF);
    }
    for (const auto &cookie : response.set_cookies()) {
        if (!HttpHeaders::valid("Set-Cookie", cookie))
            throw std::invalid_argument("Invalid cookie field");
        out.append("Set-Cookie: ").append(cookie).append(kCRLF);
    }
    if (code == 205)
        out.append("Content-Length: 0").append(kCRLF);
    else if (p.chunked)
        out.append("Transfer-Encoding: chunked").append(kCRLF);
    else if (code >= 200 && code != 204 && code != 304 &&
             p.length != HttpBody::UnknownLength)
        out.append("Content-Length: ")
            .append(std::to_string(p.length))
            .append(kCRLF);

    // 304 响应可以携带显式提供的表示长度元数据，
    // 但不发送表示正文。
    else if (code == 304 &&
             response.body_source().kind() != HttpBody::Kind::Empty &&
             p.length != HttpBody::UnknownLength)
        out.append("Content-Length: ")
            .append(std::to_string(p.length))
            .append(kCRLF);
    out.append(upgrade   ? "Connection: Upgrade"
               : p.close ? "Connection: close"
                         : "Connection: keep-alive")
        .append(kCRLF);
    out.append(kCRLF);
    return out;
}

std::string ResponseEncoder::serialize(const HttpResponse &response,
                                       bool include_body, HttpMethod method) {
    HttpRequestLine request;
    request.method = method;
    request.version = response.version();
    auto p = plan(request, response,
                  response.status_code() == HttpStatus::SWITCHING_PROTOCOLS);
    std::string out = headers(
        response, p, response.status_code() == HttpStatus::SWITCHING_PROTOCOLS);
    if (include_body && p.send_body &&
        response.body_source().kind() == HttpBody::Kind::Memory) {
        if (p.chunked) {
            out.append(chunk(response.body_content().data(),
                             response.body_content().size()))
                .append("0")
                .append(kCRLF)
                .append(kCRLF);
        } else {
            out.append(response.body_content());
        }
    }
    return out;
}

void ResponseEncoder::serialize_to(const HttpResponse &response,
                                   std::string *out, bool include_body,
                                   HttpMethod method) {
    if (out)
        *out = serialize(response, include_body, method);
}
} // namespace zhttp
