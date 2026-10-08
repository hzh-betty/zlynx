#include "zhttp/http_body.h"
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
namespace zhttp {
constexpr std::uint64_t HttpBody::UnknownLength;
FileBody::~FileBody() {
    if (fd >= 0)
        ::close(fd);
}
HttpBody::~HttpBody() = default;
HttpBody::HttpBody(HttpBody &&other) noexcept { *this = std::move(other); }
HttpBody &HttpBody::operator=(HttpBody &&other) noexcept {
    if (this == &other)
        return *this;
    kind_ = other.kind_;
    memory_ = std::move(other.memory_);
    file_ = std::move(other.file_);
    stream_ = std::move(other.stream_);
    length_ = other.length_;
    // 所有权已转移，源对象恢复为空正文，防止再次使用旧长度和回调。
    other.kind_ = Kind::Empty;
    other.length_ = 0;
    other.memory_.clear();
    other.stream_ = {};
    return *this;
}
HttpBody HttpBody::memory(std::string bytes) {
    HttpBody body;
    body.kind_ = Kind::Memory;
    body.length_ = bytes.size();
    body.memory_ = std::move(bytes);
    return body;
}
HttpBody HttpBody::file(const std::string &path, std::uint64_t offset,
                        std::uint64_t length) {
    HttpBody body;
    std::unique_ptr<FileBody> file(new FileBody);
    // 创建时打开并由 RAII 关闭描述符；后续路径改名不改变发送使用的文件。
    file->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    if (file->fd < 0 || ::fstat(file->fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || offset > static_cast<std::uint64_t>(st.st_size))
        throw std::runtime_error("Cannot open response file");
    // 先检查 offset 不越界，再以减法校验长度，避免 offset + length 溢出。
    const auto available = static_cast<std::uint64_t>(st.st_size) - offset;
    if (length == UnknownLength)
        length = available;
    if (length > available)
        throw std::invalid_argument("Response file range exceeds size");
    file->offset = offset;
    file->size = length;
    body.kind_ = Kind::File;
    body.length_ = length;
    body.file_ = std::move(file);
    return body;
}
HttpBody HttpBody::stream(StreamCallback callback, std::uint64_t length) {
    if (!callback)
        throw std::invalid_argument("Empty stream callback");
    HttpBody body;
    body.kind_ = Kind::Stream;
    body.length_ = length;
    body.stream_ = std::move(callback);
    return body;
}
} // namespace zhttp
