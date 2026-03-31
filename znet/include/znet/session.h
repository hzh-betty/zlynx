#ifndef ZNET_SESSION_H_
#define ZNET_SESSION_H_

#include "buffer.h"
#include "noncopyable.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/types.h>

#include "znet/socket.h"

namespace znet {

class TcpConnection;
class Socket;

/**
 * @brief 流抽象基类，用于解耦“网络连接”和“应用协议缓冲”。
 *
 * 设计目标：
 * - TcpConnection 负责连接状态、调度归属与流驱动；
 * - Stream 负责数据组织、协议编解码及可选 socket 收发策略；
 * - 业务回调通过 Stream 访问已接收数据或写入待发送数据。
 */
class Stream : public std::enable_shared_from_this<Stream>, public NonCopyable {
 public:
  using ptr = std::shared_ptr<Stream>;

  Stream();
  virtual ~Stream() = default;

  /**
  * @brief 流被连接激活时触发（可选重写）。
  */
  virtual void on_open() {}

  /**
  * @brief 流被连接关闭时触发（可选重写）。
  */
  virtual void on_close() {}

  /**
  * @brief 从流中读取数据。
  */
  ssize_t read(void* buffer, size_t length, uint32_t timeout_ms = 0);

  /**
  * @brief 向流中写入数据。
  */
  ssize_t write(const void* buffer, size_t length, uint32_t timeout_ms = 0);

  /**
  * @brief 获取流内待消费字节数。
  */
  virtual size_t pending_bytes() const = 0;

  /**
   * @brief 将底层连接读取到流缓冲。
   *
   * 默认实现为空操作，返回 0。
   */
  virtual ssize_t read_to_buffer(size_t max_read_bytes,
                                 uint32_t timeout_ms = 0);

  /**
   * @brief 将流缓冲刷新到底层连接。
   *
   * 默认实现为空操作，返回 0。
   */
  virtual ssize_t flush_buffer(uint32_t timeout_ms = 0);

  /**
  * @brief 绑定所属 TcpConnection。
  */
  void set_connection(const std::shared_ptr<TcpConnection>& connection);

  /**
  * @brief 获取所属 TcpConnection。
  */
  std::shared_ptr<TcpConnection> connection() const;

  /**
  * @brief 获取连接所属协程调度器 id。
  */
  int owner_sched_id() const;

 protected:
  virtual ssize_t do_read(void* buffer, size_t length,
                          uint32_t timeout_ms) = 0;
  virtual ssize_t do_write(const void* buffer, size_t length,
                           uint32_t timeout_ms) = 0;

  std::weak_ptr<TcpConnection> connection_;
};

class SocketStream : public Stream {
 public:
  using ptr = std::shared_ptr<SocketStream>;

  SocketStream() = default;
  ~SocketStream() override = default;

  ssize_t read_to_buffer(size_t max_read_bytes, uint32_t timeout_ms) override;
  ssize_t flush_buffer(uint32_t timeout_ms) override;

 protected:
  ssize_t do_read(void* buffer, size_t length, uint32_t timeout_ms) override;
  ssize_t do_write(const void* buffer, size_t length,
                   uint32_t timeout_ms) override;
  size_t pending_bytes() const override;

 Socket::ptr socket() const;
};

class BufferStream : public SocketStream {
 public:
  using ptr = std::shared_ptr<BufferStream>;

  BufferStream() = default;
  ~BufferStream() override = default;

 protected:
  /**
   * @brief 从内部 Buffer 读取数据。
   * @return >0 读取字节数；-1 表示当前无可读数据（errno=EAGAIN）或参数错误。
   */
  ssize_t do_read(void* buffer, size_t length, uint32_t timeout_ms) override;

  /**
   * @brief 将数据追加到内部 Buffer。
   */
  ssize_t do_write(const void* buffer, size_t length, uint32_t timeout_ms) override;

  /**
   * @brief 返回 Buffer 当前可读字节数。
   */
  size_t pending_bytes() const override;

  /**
   * @brief 从 socket 读取并追加到内部 Buffer。
   */
  ssize_t read_to_buffer(size_t max_read_bytes, uint32_t timeout_ms) override;

  /**
   * @brief 将内部 Buffer 数据刷到 socket。
   */
  ssize_t flush_buffer(uint32_t timeout_ms) override;

 private:
  Buffer buffer_; // 内部缓冲区，用于存储待发送或已接收的数据
};

}  // namespace znet

#endif  // ZNET_SESSION_H_
