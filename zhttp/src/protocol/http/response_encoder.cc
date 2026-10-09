#include "protocol/http/response_encoder.h"
#include <cstdio>
#include <stdexcept>
namespace zhttp {

std::string ResponseEncoder::chunk(const char *data, std::size_t size) {
    if (!size)
        return "";
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "%zx\r\n", size);
    std::string frame(line, static_cast<size_t>(n));
    frame.append(data, size);
    frame.append("\r\n");
    return frame;
}


ResponsePlan ResponseEncoder::plan(const HttpRequestLine &request,
                                      const HttpResponse &response,
                                      bool upgrade) {
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
    for (const auto &value : response.headers().get_all("Connection"))
        if (header_contains_token(value, "close"))
            p.close = true;
    return p;
}
std::string ResponseEncoder::headers(const HttpRequestLine &request,
                                        const HttpResponse &response,
                                        bool upgrade) {
    auto p = plan(request, response, upgrade);
    const auto code = response.status_line().status_code;
    std::string out = std::string(version_to_string(response.version())) + " " +
                      std::to_string(code) + " " +
                      status_to_string(response.status_code()) + "\r\n";
    // 正文边界和连接头由写出计划统一生成，忽略业务层设置的同名字段。
    for (const auto &h : response.headers()) {
        if (HttpHeaders::equal_name(h.first, "Content-Length") ||
            HttpHeaders::equal_name(h.first, "Transfer-Encoding") ||
            HttpHeaders::equal_name(h.first, "Connection"))
            continue;
        out += h.first + ": " + h.second + "\r\n";
    }
    for (const auto &cookie : response.set_cookies()) {
        if (!HttpHeaders::valid("Set-Cookie", cookie))
            throw std::invalid_argument("Invalid cookie field");
        out += "Set-Cookie: " + cookie + "\r\n";
    }
    if (code == 205)
        out += "Content-Length: 0\r\n";
    else if (p.chunked)
        out += "Transfer-Encoding: chunked\r\n";
    else if (code >= 200 && code != 204 && code != 304 &&
             p.length != HttpBody::UnknownLength)
        out += "Content-Length: " + std::to_string(p.length) + "\r\n";
    // A 304 with an explicitly supplied representation is permitted length
    // metadata.
    else if (code == 304 &&
             response.body_source().kind() != HttpBody::Kind::Empty &&
             p.length != HttpBody::UnknownLength)
        out += "Content-Length: " + std::to_string(p.length) + "\r\n";
    out += upgrade ? "Connection: Upgrade\r\n"
                   : "Connection: " +
                         std::string(p.close ? "close" : "keep-alive") + "\r\n";
    out += "\r\n";
    return out;
}
std::string ResponseEncoder::serialize(const HttpResponse &response,
                                          bool include_body,
                                          HttpMethod method) {
    HttpRequestLine request;
    request.method = method;
    request.version = response.version();
    std::string out =
        headers(request, response,
                response.status_code() == HttpStatus::SWITCHING_PROTOCOLS);
    auto p = plan(request, response,
                  response.status_code() == HttpStatus::SWITCHING_PROTOCOLS);
    if (include_body && p.send_body &&
        response.body_source().kind() == HttpBody::Kind::Memory) {
        out += p.chunked ? chunk(response.body_content().data(),
                                             response.body_content().size()) +
                               "0\r\n\r\n"
                         : response.body_content();
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
