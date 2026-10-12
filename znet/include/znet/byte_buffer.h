#ifndef ZLYNX_ZNET_INCLUDE_ZNET_BYTE_BUFFER_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_BYTE_BUFFER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace znet {
/**
 * @brief 拥有连续协议字节的缓冲区，不依赖 I/O 或运行时。
 * @note 不提供内部同步，由调用方串行化修改。
 * peek()、begin_write()、view() 和 find_crlf() 的结果均借用内部存储。
 * append() 或 ensure_writable_bytes()
 * 搬移或重分配数据时，须重新获取指针和视图；
 * 消费数据后旧视图不再表示当前可读区间，赋值或销毁对象也可能使借用失效。
 */
class ByteBuffer {
  public:
    explicit ByteBuffer(size_t capacity = 1024);
    ByteBuffer(const ByteBuffer &) = default;
    ByteBuffer &operator=(const ByteBuffer &) = default;

    size_t readable_bytes() const { return writer_ - reader_; }

    size_t writable_bytes() const { return bytes_.size() - writer_; }

    const char *peek() const { return bytes_.data() + reader_; }

    char *begin_write() { return bytes_.data() + writer_; }

    std::string_view view() const { return {peek(), readable_bytes()}; }

    const char *find_crlf() const;
    void append(std::string_view bytes);

    /**
     * @brief 复制并追加原始字节，支持从自身存储追加。
     * @pre size 非零时 bytes 须指向至少 size 字节的可读数据。
     * @throws std::invalid_argument size 非零但 bytes 为空。
     */
    void append(const void *bytes, size_t size);
    void retrieve(size_t size);

    void retrieve_all() { reader_ = writer_ = 0; }

    std::string retrieve_as_string(size_t size);

    std::string retrieve_all_as_string() {
        return retrieve_as_string(readable_bytes());
    }

    /**
     * @brief 保留至少 size 字节的可写空间，必要时搬移或重分配存储。
     * @note 调用后重新获取写入指针；此操作不增加可读字节数。
     */
    void ensure_writable_bytes(size_t size);

    /**
     * @brief 将此前写入的 size 字节纳入可读区间。
     * @pre 调用方已写入相应字节。
     * @throws std::out_of_range size 超过当前可写容量。
     */
    void has_written(size_t size);

  private:
    std::vector<char> bytes_;
    size_t reader_ = 0, writer_ = 0;
};
} // namespace znet

#endif
