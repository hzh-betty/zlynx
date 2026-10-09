#include "zhttp/writer/http_response_writer.h"
#include "znet/server/connection.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace zhttp {
namespace {
std::string encode_http_chunk(const char *data, std::size_t size) {
    if (!size)
        return "";
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "%zx\r\n", size);
    std::string frame(line, static_cast<size_t>(n));
    frame.append(data, size);
    frame.append("\r\n");
    return frame;
}
} // namespace

bool send_all_or_fail(const std::shared_ptr<znet::Connection> &conn,
                      const char *data, size_t size) {
    if (!conn || !conn->connected())
        return false;
    // 只在连接回调内调用；每批数据排空后才继续生产，避免积压响应内容。
    while (size) {
        const size_t batch = std::min(size, static_cast<size_t>(64 * 1024));
        auto sent = conn->send(std::string_view(data, batch));
        if (!sent || sent.bytes != batch)
            return false;
        data += batch;
        size -= batch;
    }
    return true;
}
ResponsePlan HttpResponseWriter::plan(const HttpRequestLine &request,
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
std::string HttpResponseWriter::headers(const HttpRequestLine &request,
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
std::string HttpResponseWriter::serialize(const HttpResponse &response,
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
        out += p.chunked ? encode_http_chunk(response.body_content().data(),
                                             response.body_content().size()) +
                               "0\r\n\r\n"
                         : response.body_content();
    }
    return out;
}
void HttpResponseWriter::serialize_to(const HttpResponse &response,
                                      std::string *out, bool include_body,
                                      HttpMethod method) {
    if (out)
        *out = serialize(response, include_body, method);
}
WriteResult HttpResponseWriter::send(
    const std::shared_ptr<znet::Connection> &conn, HttpContext &context,
    bool upgrade) {
    auto &response = context.response();
    if (response.committed())
        throw std::logic_error("Response already committed");
    auto p = plan(context.request().request_line(), response, upgrade);
    const auto &body = response.body_source();
    if (body.kind() == HttpBody::Kind::File) {
        const auto *file = body.file_resource();
        struct stat st{};
        if (::fstat(file->fd, &st) != 0 || st.st_size < 0 ||
            file->offset > static_cast<std::uint64_t>(st.st_size) ||
            file->size > static_cast<std::uint64_t>(st.st_size) - file->offset)
            throw std::runtime_error("Response file changed before commit");
    }
    const auto header_bytes =
        headers(context.request().request_line(), response, upgrade);
    // 文件范围与头部在提交前校验；提交后只允许发送或失败，不能改写响应。
    response.commit();
    if (!send_all_or_fail(conn, header_bytes.data(), header_bytes.size()))
        return WriteResult::Failed;
    if (!p.send_body)
        return WriteResult::Completed;
    if (body.kind() == HttpBody::Kind::Stream) {
        char buffer[8192];
        std::uint64_t sent = 0;
        // 回调与写出在同一次连接处理里完成，不依赖新的读取事件或控制队列。
        while (conn && conn->connected()) {
            std::size_t count;
            try {
                count = body.stream_callback()(buffer, sizeof(buffer));
            } catch (...) {
                return WriteResult::Failed;
            }
            if (count == 0) {
                // 已声明长度时，提前结束也属于失败，不能把截断响应标为完成。
                if (p.length != HttpBody::UnknownLength && sent != p.length)
                    return WriteResult::Failed;
                break;
            }
            if (count > sizeof(buffer) ||
                (p.length != HttpBody::UnknownLength && count > p.length - sent))
                return WriteResult::Failed;
            const auto bytes = p.chunked ? encode_http_chunk(buffer, count)
                                        : std::string(buffer, count);
            if (!send_all_or_fail(conn, bytes.data(), bytes.size()))
                return WriteResult::Failed;
            sent += count;
        }
        if (!conn || !conn->connected())
            return WriteResult::Failed;
    } else if (body.kind() == HttpBody::Kind::File) {
        const auto *file = body.file_resource();
        std::uint64_t sent = 0;
        char buffer[8192];
        while (sent < file->size) {
            auto n = ::pread(
                file->fd, buffer,
                std::min<std::uint64_t>(sizeof(buffer), file->size - sent),
                file->offset + sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return WriteResult::Failed;
            auto bytes = p.chunked ? encode_http_chunk(buffer, n)
                                   : std::string(buffer, n);
            if (!send_all_or_fail(conn, bytes.data(), bytes.size()))
                return WriteResult::Failed;
            sent += n;
        }
    } else {
        const auto &bytes = body.content();
        for (size_t sent = 0; sent < bytes.size();) {
            const auto count =
                std::min(bytes.size() - sent, static_cast<size_t>(64 * 1024));
            if (p.chunked) {
                const auto chunk =
                    encode_http_chunk(bytes.data() + sent, count);
                if (!send_all_or_fail(conn, chunk.data(), chunk.size()))
                    return WriteResult::Failed;
            } else if (!send_all_or_fail(conn, bytes.data() + sent, count))
                return WriteResult::Failed;
            sent += count;
        }
    }
    // 正文完全发送后才写终止 chunk；失败路径不伪造正常结束标记。
    if (p.chunked && !send_all_or_fail(conn, "0\r\n\r\n", 5))
        return WriteResult::Failed;
    return WriteResult::Completed;
}
} // namespace zhttp
