#include "protocol/http/http_response_writer.h"
#include "protocol/http/response_encoder.h"
#include "znet/server/connection.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace zhttp {
static bool send_all_or_fail(const std::shared_ptr<znet::Connection> &conn,
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
WriteResult HttpResponseWriter::send(
    const std::shared_ptr<znet::Connection> &conn, HttpContext &context,
    bool upgrade) {
    auto &response = context.response();
    if (response.committed())
        throw std::logic_error("Response already committed");
    auto p = ResponseEncoder::plan(context.request().request_line(), response, upgrade);
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
        ResponseEncoder::headers(context.request().request_line(), response, upgrade);
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
            const auto bytes = p.chunked ? ResponseEncoder::chunk(buffer, count)
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
            auto bytes = p.chunked ? ResponseEncoder::chunk(buffer, n)
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
                    ResponseEncoder::chunk(bytes.data() + sent, count);
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
