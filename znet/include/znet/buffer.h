#ifndef ZNET_BUFFER_H_
#define ZNET_BUFFER_H_

#include "noncopyable.h"

#include <cstddef>
#include <string>
#include <vector>

namespace znet {

/**
 * @brief 轻量级字节缓冲区，采用“可预留头部 + 线性可读写区”布局。
 *
 * 内部索引约定：
 * - [0, reader_index_)：已回收区，其中前 kCheapPrepend 字节预留给协议头回填。
 * - [reader_index_, writer_index_)：当前可读数据区。
 * - [writer_index_, data_.size())：当前可写空闲区。
 *
 * 设计目标：
 * - 为网络读写提供低拷贝、可增长的临时缓冲；
 * - 在常见“小包 + 追加写”场景下避免频繁 realloc；
 * - 通过 read_fd/write_fd 直接与 fd 交互，减少中间层样板代码。
 */
class Buffer : public NonCopyable {
 public:
  // 预留给协议头回填的前置空间大小（字节）。
  static const size_t kCheapPrepend = 8;

  // 默认初始可写容量（不含前置预留区）。
  static const size_t kInitialSize = 1024;

  /**
   * @brief 构造缓冲区。
   * @param initial_size 初始可写容量（不含 kCheapPrepend）。
   */
  explicit Buffer(size_t initial_size = kInitialSize);

  /**
   * @brief 查找 CRLF（\r\n）位置，返回指向 CRLF 首字符的指针。
   * @return 找到时返回指向 CRLF 首字符的指针，未找到时返回 nullptr。
   */
  const char* find_crlf() const;

  /**
   * @brief 当前可读字节数。
   */
  size_t readable_bytes() const;

  /**
   * @brief 当前可写字节数。
   */
  size_t writable_bytes() const;

  /**
   * @brief 当前可前置写入的字节数。
   *
   * 通常用于在已写入 payload 后回填长度字段、协议头等。
   */
  size_t prependable_bytes() const;

  /**
   * @brief 返回可读区起始地址。
   * @return 当无可读数据时返回 nullptr。
   */
  const char* peek() const;

  /**
   * @brief 消费指定长度的可读数据。
   *
   * 若 length 大于等于当前可读字节数，则等价于 retrieve_all()。
   */
  void retrieve(size_t length);

  /**
   * @brief 清空可读数据并重置读写索引。
   */
  void retrieve_all();

  /**
   * @brief 取出指定长度数据并转换为字符串。
   *
   * 实际取出长度为 min(length, readable_bytes())。
   */
  std::string retrieve_as_string(size_t length);

  /**
   * @brief 取出全部可读数据并转换为字符串。
   */
  std::string retrieve_all_as_string();

  /**
   * @brief 追加二进制数据。
   * @param data 输入数据首地址。
   * @param length 输入长度。
   */
  void append(const void* data, size_t length);

  /**
   * @brief 追加字符数据。
   */
  void append(const char* data, size_t length);

  /**
   * @brief 追加字符串数据。
   */
  void append(const std::string& data);

  /**
   * @brief 获取当前可写区起始地址（可写版本）。
   */
  char* begin_write();

  /**
   * @brief 获取当前可写区起始地址（只读版本）。
   */
  const char* begin_write() const;

  /**
   * @brief 声明“已写入 length 字节”，推进写索引。
   */
  void has_written(size_t length);

  /**
   * @brief 确保至少存在 length 字节可写空间。
   *
   * 若当前空间不足，内部会通过 make_space() 做“搬移”或“扩容”。
   */
  void ensure_writable_bytes(size_t length);

  /**
   * @brief 从 fd 读取数据并追加到缓冲区。
   * @param fd 目标文件描述符。
   * @param saved_errno 失败时写回 errno，可为空。
   * @return readv 返回值：>0 读取字节数，0 对端关闭，<0 读取失败。
   */
  ssize_t read_fd(int fd, int* saved_errno);

  /**
   * @brief 将当前可读数据写入 fd。
   * @param fd 目标文件描述符。
   * @param saved_errno 失败时写回 errno，可为空。
   * @return write 返回值：>0 写出字节数，<0 写入失败。
   */
  ssize_t write_fd(int fd, int* saved_errno);

 private:
  // 返回内部连续内存首地址。
  char* begin();

  // 返回内部连续内存首地址（只读版本）。
  const char* begin() const;

  // 为追加写制造空间：优先搬移可读区，不够时再扩容。
  void make_space(size_t length);

  // 实际存储容器，布局由 reader_index_/writer_index_ 管理。
  std::vector<char> data_;

  // 可读区起始索引。
  size_t reader_index_;

  // 可读区结束且可写区起始索引。
  size_t writer_index_;
};

}  // namespace znet

#endif  // ZNET_BUFFER_H_
