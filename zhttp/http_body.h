#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
namespace zhttp {
// 拥有已打开文件的资源寿命，路径删除或改名不会改变发送所用的描述符。
struct FileBody {
    FileBody() = default;
    FileBody(const FileBody &) = delete;
    FileBody &operator=(const FileBody &) = delete;
    int fd = -1;
    std::uint64_t offset = 0, size = 0;
    ~FileBody();
};
class HttpBody {
  public:
    // 拉取回调只在当前请求的写出过程中调用；返回 0 表示数据结束。
    using StreamCallback = std::function<std::size_t(char *, std::size_t)>;
    enum class Kind { Empty, Memory, File, Stream };
    static constexpr std::uint64_t UnknownLength =
        std::numeric_limits<std::uint64_t>::max();
    HttpBody() = default;
    ~HttpBody();
    HttpBody(HttpBody &&other) noexcept;
    HttpBody &operator=(HttpBody &&other) noexcept;
    HttpBody(const HttpBody &) = delete;
    HttpBody &operator=(const HttpBody &) = delete;
    static HttpBody memory(std::string bytes);
    static HttpBody file(const std::string &path, std::uint64_t offset = 0,
                         std::uint64_t length = UnknownLength);
    static HttpBody stream(StreamCallback callback,
                           std::uint64_t length = UnknownLength);
    Kind kind() const { return kind_; }
    bool length_known() const { return length_ != UnknownLength; }
    std::uint64_t length() const { return length_; }
    const std::string &content() const { return memory_; }
    const FileBody *file_resource() const { return file_.get(); }
    const StreamCallback &stream_callback() const { return stream_; }

  private:
    Kind kind_ = Kind::Empty;
    std::string memory_;
    std::unique_ptr<FileBody> file_;
    StreamCallback stream_;
    std::uint64_t length_ = 0;
};
} // namespace zhttp
