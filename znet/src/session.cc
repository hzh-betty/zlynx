#include "znet/session.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "znet/socket.h"
#include "znet/tcp_connection.h"
#include "znet/znet_logger.h"

namespace znet {

// Stream 仅提供统一接口和连接弱引用，不直接承载缓冲策略。
Stream::Stream() : connection_() {}

ssize_t Stream::read(void* buffer, size_t length, uint32_t timeout_ms) {
  // 统一入口，具体行为由派生流实现。
  return do_read(buffer, length, timeout_ms);
}

ssize_t Stream::write(const void* buffer, size_t length, uint32_t timeout_ms) {
  // 统一入口，具体行为由派生流实现。
  return do_write(buffer, length, timeout_ms);
}

ssize_t Stream::read_to_buffer(size_t /*max_read_bytes*/, uint32_t /*timeout_ms*/) {
  // 基类默认“无内部缓冲”，由子类按需覆盖。
  return 0;
}

ssize_t Stream::flush_buffer(uint32_t /*timeout_ms*/) {
  // 基类默认“无待刷数据”，由子类按需覆盖。
  return 0;
}

void Stream::set_connection(const std::shared_ptr<TcpConnection>& connection) {
  connection_ = connection;
}

std::shared_ptr<TcpConnection> Stream::connection() const {
  return connection_.lock();
}

int Stream::owner_sched_id() const {
  std::shared_ptr<TcpConnection> conn = connection();
  if (!conn) {
    return -1;
  }
  return conn->owner_sched_id();
}

// 从关联连接获取底层 socket，并统一做有效性检查。
std::shared_ptr<Socket> SocketStream::socket() const {
  const std::shared_ptr<TcpConnection> conn = connection();
  if (!conn) {
    errno = ENOTCONN;
    return nullptr;
  }

 Socket::ptr sock = conn->socket();
  if (!sock || !sock->is_valid()) {
    errno = EBADF;
    return nullptr;
  }
  return sock;
}

// 直接透传到底层 socket::recv；timeout_ms>0 时先设置接收超时。
ssize_t SocketStream::do_read(void* buffer, size_t length, uint32_t timeout_ms) {
  if (!buffer && length > 0) {
    errno = EINVAL;
    ZNET_LOG_WARN("SocketStream::do_read received null output buffer with "
                  "length={}",
                  length);
    return -1;
  }

  if (length == 0) {
    return 0;
  }

 Socket::ptr sock = socket();
  if (!sock) {
    return -1;
  }

  if (timeout_ms > 0) {
    (void)sock->set_recv_timeout(timeout_ms);
  }

  return sock->recv(buffer, length);
}

// 直接透传到底层 socket::send；timeout_ms>0 时先设置发送超时。
ssize_t SocketStream::do_write(const void* buffer, size_t length,
                               uint32_t timeout_ms) {
  if (!buffer && length > 0) {
    errno = EINVAL;
    ZNET_LOG_WARN("SocketStream::do_write received null input buffer with "
                  "length={}",
                  length);
    return -1;
  }

  if (length == 0) {
    return 0;
  }

 Socket::ptr sock = socket();
  if (!sock) {
    return -1;
  }

  if (timeout_ms > 0) {
    (void)sock->set_send_timeout(timeout_ms);
  }

  return sock->send(buffer, length);
}

size_t SocketStream::pending_bytes() const {
  // 纯 socket 流不维护用户态待发送缓冲。
  return 0;
}

// 通过 MSG_PEEK 探测可读数据，不消费内核接收缓冲。
ssize_t SocketStream::read_to_buffer(size_t max_read_bytes,
                                     uint32_t timeout_ms) {
  if (max_read_bytes == 0) {
    errno = EINVAL;
    return -1;
  }

 Socket::ptr sock = socket();
  if (!sock) {
    return -1;
  }

  if (timeout_ms > 0) {
    (void)sock->set_recv_timeout(timeout_ms);
  }

  std::vector<char> probe(max_read_bytes);
  return sock->recv(probe.data(), probe.size(), MSG_PEEK);
}

ssize_t SocketStream::flush_buffer(uint32_t /*timeout_ms*/) {
  // 纯 socket 流无用户态发送缓冲，因此无需 flush。
  return 0;
}

// 从内部 Buffer 读取数据：无数据时返回 EAGAIN，保持非阻塞语义。
ssize_t BufferStream::do_read(void* buffer, size_t length, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (!buffer) {
    errno = EINVAL;
    ZNET_LOG_WARN("BufferStream::do_read received null output buffer");
    return -1;
  }

  if (buffer_.readable_bytes() == 0) {
    // 约定：BufferStream 无数据时返回 EAGAIN，而不是阻塞等待。
    errno = EAGAIN;
    return -1;
  }

  const size_t read_len = std::min(length, buffer_.readable_bytes());
  memcpy(buffer, buffer_.peek(), read_len);
  buffer_.retrieve(read_len);
  return static_cast<ssize_t>(read_len);
}

// 将应用层数据追加到内部 Buffer，真正发送由 flush_buffer() 驱动。
ssize_t BufferStream::do_write(const void* buffer, size_t length,
                                uint32_t timeout_ms) {
  (void)timeout_ms;
  if (!buffer) {
    errno = EINVAL;
    ZNET_LOG_WARN("BufferStream::do_write received null input buffer");
    return -1;
  }

  buffer_.append(buffer, length);
  return static_cast<ssize_t>(length);
}

size_t BufferStream::pending_bytes() const {
  return buffer_.readable_bytes();
}

// 从 socket 读取数据并缓存在内部 Buffer，便于上层分批消费。
ssize_t BufferStream::read_to_buffer(size_t max_read_bytes,
                                     uint32_t timeout_ms) {
  if (max_read_bytes == 0) {
    errno = EINVAL;
    return -1;
  }

 Socket::ptr sock = socket();
  if (!sock) {
    return -1;
  }

  if (timeout_ms > 0) {
    (void)sock->set_recv_timeout(timeout_ms);
  }

  std::vector<char> data(max_read_bytes);
  const ssize_t n = sock->recv(data.data(), data.size());
  if (n > 0) {
    // 仅在成功读到字节时追加，避免将未初始化内容写入 buffer_。
    buffer_.append(data.data(), static_cast<size_t>(n));
  }
  return n;
}

// 尽力将内部 Buffer 数据写出；遇到可重试错误返回已发送字节或暂时退出。
ssize_t BufferStream::flush_buffer(uint32_t timeout_ms) {
 Socket::ptr sock = socket();
  if (!sock) {
    return -1;
  }

  if (timeout_ms > 0) {
    (void)sock->set_send_timeout(timeout_ms);
  }

  ssize_t sent_total = 0;
  while (buffer_.readable_bytes() > 0) {
    const ssize_t n = sock->send(buffer_.peek(), buffer_.readable_bytes());
    if (n > 0) {
      buffer_.retrieve(static_cast<size_t>(n));
      sent_total += n;
      continue;
    }

    if (n == 0) {
      break;
    }

    if (errno == EINTR) {
      continue;
    }

    // 发送缓冲暂时不可写：保留剩余数据，交由后续时机继续 flush。
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    // 出现不可恢复错误：若已有部分写出则返回写出量，否则返回失败。
    if (sent_total > 0) {
      return sent_total;
    }
    return -1;
  }

  return sent_total;
}

}  // namespace znet
