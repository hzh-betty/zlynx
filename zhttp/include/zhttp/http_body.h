#ifndef ZHTTP_HTTP_BODY_H_
#define ZHTTP_HTTP_BODY_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
namespace zhttp {
// 拥有已打开文件的资源寿命，路径删除或改名不会改变发送所用的描述符。
/** 文件正文资源；析构时关闭 fd，独占文件描述符所有权。 */
struct FileBody {
    FileBody() = default;
    FileBody(const FileBody &) = delete;
    FileBody &operator=(const FileBody &) = delete;
    int fd = -1;
    std::uint64_t offset = 0, size = 0;
    ~FileBody();
};
/**
 * 可移动、不可复制的正文数据源。
 *
 * 支持内存、已打开文件和同步拉取流；移动后原对象恢复为空正文。
 */
class HttpBody {
  public:
    // 拉取回调只在当前请求的写出过程中调用；返回 0 表示数据结束。
    /**
     * 同步拉取正文的回调。
     * 第一个参数为待填充缓冲区，第二个参数为可写容量（字节）。
     * 注意：回调在连接处理过程中执行，不能保存缓冲区或跨线程写出。
     *
     * @return 实际填充字节数，0 表示结束；不得超过缓冲区容量。
     */
    using StreamCallback = std::function<std::size_t(char *, std::size_t)>;
    /** 正文存储类型。 */
    enum class Kind { Empty, Memory, File, Stream };
    /** 流长度未知的哨兵值。 */
    static constexpr std::uint64_t UnknownLength =
        std::numeric_limits<std::uint64_t>::max();
    /** 构造长度为 0 的空正文。 */
    HttpBody() = default;
    /** 释放正文及其持有的文件资源。 */
    ~HttpBody();
    /** 转移正文所有权，原对象置空。 */
    HttpBody(HttpBody &&other) noexcept;
    /** 释放当前正文并接管源对象资源，支持自移动。 */
    HttpBody &operator=(HttpBody &&other) noexcept;
    HttpBody(const HttpBody &) = delete;
    HttpBody &operator=(const HttpBody &) = delete;
    /**
     * 接管内存字节串，长度由 bytes.size() 确定。
     *
     * @param bytes 正文字节。
     * @return 拥有字节串的正文对象。
     */
    static HttpBody memory(std::string bytes);
    /**
     * 打开普通文件并持有指定范围的正文。
     *
     * @param path 文件路径；创建时打开，析构时关闭。
     * @param offset 起始字节偏移。
     * @param length 字节数；UnknownLength 表示从 offset 到文件末尾。
     * @return 持有文件描述符和范围的正文对象。
     * @throws std::runtime_error 打开、文件类型或偏移校验失败。
     * @throws std::invalid_argument 指定长度超出文件范围。
     */
    static HttpBody file(const std::string &path, std::uint64_t offset = 0,
                         std::uint64_t length = UnknownLength);
    /**
     * 创建同步拉取流正文。
     *
     * @param callback 非空拉取回调。
     * @param length 预期总字节数；UnknownLength 表示未知。
     * @return 流正文对象；已知长度必须与回调产出的总长度一致。
     * @throws std::invalid_argument 回调为空。
     */
    static HttpBody stream(StreamCallback callback,
                           std::uint64_t length = UnknownLength);
    /** 返回正文存储类型。 */
    Kind kind() const { return kind_; }
    /** 返回是否具有确定长度。 */
    bool length_known() const { return length_ != UnknownLength; }
    /** 返回正文字节数；未知长度返回 UnknownLength。 */
    std::uint64_t length() const { return length_; }
    /** 返回内存正文；其他类型不读取文件或调用流回调。 */
    const std::string &content() const { return memory_; }
    /** 返回借用的文件资源指针；非文件正文返回 nullptr。 */
    const FileBody *file_resource() const { return file_.get(); }
    /** 返回借用的流回调引用；仅 Stream 类型具有有效回调。 */
    const StreamCallback &stream_callback() const { return stream_; }

  private:
    Kind kind_ = Kind::Empty;
    std::string memory_;
    std::unique_ptr<FileBody> file_;
    StreamCallback stream_;
    std::uint64_t length_ = 0;
};
} // namespace zhttp

#endif // ZHTTP_HTTP_BODY_H_
