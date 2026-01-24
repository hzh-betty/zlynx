#ifndef ZNET_BUFFER_H_
#define ZNET_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace znet {

/**
 * @brief 网络缓冲区，类 muduo Buffer 设计
 *
 * 内部结构：
 * +-------------------+------------------+------------------+
 * | prependable bytes |  readable bytes  |  writable bytes  |
 * |                   |     (CONTENT)    |                  |
 * +-------------------+------------------+------------------+
 * |                   |                  |                  |
 * 0      <=      readerIndex   <=   writerIndex    <=     size
 */
class Buffer {
public:
  static const size_t kCheapPrepend = 8;
  static const size_t kInitialSize = 1024;

  /**
   * @brief 构造函数
   * @param initial_size 初始缓冲区大小
   */
  explicit Buffer(size_t initial_size = kInitialSize);

  ~Buffer();

  /**
   * @brief 可读字节数
   */
  size_t readable_bytes() const { return writer_index_ - reader_index_; }

  /**
   * @brief 可写字节数
   */
  size_t writable_bytes() const { return capacity_ - writer_index_; }

  /**
   * @brief 预留空间字节数
   */
  size_t prependable_bytes() const { return reader_index_; }

  /**
   * @brief 总容量
   */
  size_t capacity() const { return capacity_; }

  /**
   * @brief 获取可读数据起始指针（只读）
   */
  const char *peek() const { return begin() + reader_index_; }

  /**
   * @brief 读取 len 字节数据到 data
   * @return 实际读取的字节数
   */
  size_t read(void *data, size_t len);

  /**
   * @brief 读取 int8_t
   */
  int8_t read_int8();

  /**
   * @brief 读取 int16_t (网络字节序)
   */
  int16_t read_int16();

  /**
   * @brief 读取 int32_t (网络字节序)
   */
  int32_t read_int32();

  /**
   * @brief 读取 int64_t (网络字节序)
   */
  int64_t read_int64();

  /**
   * @brief 读取指定长度的字符串
   */
  std::string read_string(size_t len);

  /**
   * @brief 读取所有可读数据为字符串
   */
  std::string read_all_as_string();

  /**
   * @brief 丢弃 len 字节
   */
  void retrieve(size_t len);

  /**
   * @brief 丢弃所有可读数据
   */
  void retrieve_all();

  /**
   * @brief 确保可写空间至少为 len 字节
   */
  void ensure_writable_bytes(size_t len);

  /**
   * @brief 写入数据
   */
  void append(const void *data, size_t len);

  /**
   * @brief 写入字符串
   */
  void append(const std::string &str);

  /**
   * @brief 写入 int8_t
   */
  void write_int8(int8_t x);

  /**
   * @brief 写入 int16_t (网络字节序)
   */
  void write_int16(int16_t x);

  /**
   * @brief 写入 int32_t (网络字节序)
   */
  void write_int32(int32_t x);

  /**
   * @brief 写入 int64_t (网络字节序)
   */
  void write_int64(int64_t x);

  /**
   * @brief 已写入 len 字节，移动 writer_index_
   */
  void has_written(size_t len);

  /**
   * @brief 查找 CRLF (\r\n)
   * @return CRLF 的起始位置，未找到返回 nullptr
   */
  const char *find_crlf() const;

  /**
   * @brief 查找 EOL (\n)
   * @return EOL 的位置，未找到返回 nullptr
   */
  const char *find_eol() const;

  /**
   * @brief 从 fd 读取数据到 buffer
   * @param fd 文件描述符
   * @param saved_errno 保存错误码
   * @return 读取的字节数，-1 表示错误
   * @note 使用 readv 结合栈上缓冲区，减少系统调用
   */
  ssize_t read_fd(int fd, int *saved_errno);

  /**
   * @brief 从 buffer 写入数据到 fd
   * @param fd 文件描述符
   * @param saved_errno 保存错误码
   * @return 写入的字节数，-1 表示错误
   */
  ssize_t write_fd(int fd, int *saved_errno);

  /**
   * @brief 获取可写区域起始指针
   */
  char *begin_write() { return begin() + writer_index_; }
  const char *begin_write() const { return begin() + writer_index_; }

  /**
   * @brief 收缩缓冲区，释放多余空间
   */
  void shrink(size_t reserve = 0);

  /**
   * @brief 交换两个 Buffer
   */
  void swap(Buffer &rhs);

private:
  char *begin() { return buffer_; }
  const char *begin() const { return buffer_; }

  /**
   * @brief 扩容
   */
  void make_space(size_t len);

private:
  char *buffer_;        // 缓冲区指针（由 Allocator 分配）
  size_t capacity_;     // 总容量
  size_t reader_index_; // 读索引
  size_t writer_index_; // 写索引
};

} // namespace znet

#endif // ZNET_BUFFER_H_
