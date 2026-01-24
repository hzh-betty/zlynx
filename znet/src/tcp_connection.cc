#include "tcp_connection.h"
#include "io_scheduler.h"
#include "scheduler.h"
#include "znet_logger.h"
#include <errno.h>
#include <mutex>
#include <string.h>

namespace znet {

const char *TcpConnection::state_to_string(State s) {
  switch (s) {
  case State::Connecting:
    return "Connecting";
  case State::Connected:
    return "Connected";
  case State::Disconnecting:
    return "Disconnecting";
  case State::Disconnected:
    return "Disconnected";
  default:
    return "Unknown";
  }
}

TcpConnection::TcpConnection(std::string name, Socket::ptr socket,
                             const Address::ptr &local_addr,
                             const Address::ptr &peer_addr,
                             zcoroutine::IoScheduler *io_scheduler)
    : name_(std::move(name)), state_(State::Connecting),
      socket_(std::move(socket)), local_addr_(local_addr),
      peer_addr_(peer_addr), io_scheduler_(io_scheduler) {

  ZNET_LOG_INFO("TcpConnection::TcpConnection [{}] fd={} local={} peer={}",
                name_, socket_->fd(), local_addr_->to_string(),
                peer_addr_->to_string());
}

TcpConnection::~TcpConnection() {
  ZNET_LOG_DEBUG("TcpConnection::~TcpConnection [{}] fd={} state={}", name_,
                 socket_->fd(),
                 state_to_string(state_.load(std::memory_order_acquire)));

  // 防御性清理：如果连接还没有关闭，移除 epoll 事件
  if (io_scheduler_ && !disconnected()) {
    socket_->close();
  }
}

void TcpConnection::connect_established() {
  set_state(State::Connected);

  // 设置Socket为非阻塞模式
  socket_->set_non_blocking(true);

  // 注册读事件到IoScheduler
  if (io_scheduler_) {
    auto self = shared_from_this();
    io_scheduler_->add_event(socket_->fd(), zcoroutine::FdContext::kRead,
                             [self]() { self->handle_read(); });
  }

  // 触发连接建立回调（异步调度）
  if (connection_callback_) {
    auto self = shared_from_this();
    auto cb = connection_callback_;
    if (zcoroutine::Scheduler::get_this() == io_scheduler_) {
      cb(self);
    } else if (io_scheduler_) {
      io_scheduler_->schedule(
          [self, cb = std::move(cb)]() mutable { cb(self); });
    } else {
      cb(self);
    }
  }

  ZNET_LOG_INFO("TcpConnection::connect_established [{}] fd={}", name_,
                socket_->fd());
}

void TcpConnection::send(const void *data, size_t len) {
  if (state_.load(std::memory_order_acquire) != State::Connected) {
    ZNET_LOG_WARN("TcpConnection::send [{}] not connected, state={}", name_,
                  state_to_string(state_.load(std::memory_order_acquire)));
    return;
  }

  send_in_loop(data, len);
}

void TcpConnection::send(const std::string &message) {
  send(message.data(), message.size());
}

void TcpConnection::send(Buffer *buf) {
  send(buf->peek(), buf->readable_bytes());
  buf->retrieve_all();
}

void TcpConnection::shutdown() {
  State expected = State::Connected;
  if (state_.compare_exchange_strong(expected, State::Disconnecting,
                                     std::memory_order_acq_rel)) {
    shutdown_in_loop();
  }
}

void TcpConnection::force_close() {
  State current = state_.load(std::memory_order_acquire);
  if (current == State::Connected || current == State::Disconnecting) {
    state_.store(State::Disconnecting, std::memory_order_release);
    force_close_in_loop();
  }
}

void TcpConnection::set_tcp_no_delay(bool on) { socket_->set_tcp_nodelay(on); }

void TcpConnection::set_keep_alive(bool on) { socket_->set_keep_alive(on); }

void TcpConnection::handle_read() {
  if (!connected()) {
    return;
  }

  // IoScheduler 的 trigger_event 会在触发前把 READ 事件从 epoll 中移除。
  // 同时 EpollPoller 强制使用 EPOLLET（边缘触发）。
  // 因此这里必须：
  // 1) 读到 EAGAIN/EWOULDBLOCK（drain），否则 ET 下可能丢事件；
  // 2) 回调返回前重新注册 READ，否则后续数据不会再触发。
  int saved_errno = 0;
  ssize_t n = 0;
  ssize_t total = 0;

  while (true) {
    n = input_buffer_.read_fd(socket_->fd(), &saved_errno);
    if (n > 0) {
      total += n;
      continue;
    }

    if (n == 0) {
      ZNET_LOG_INFO("TcpConnection::handle_read [{}] peer closed", name_);
      handle_close();
      return;
    }

    // n < 0
    if (saved_errno == EINTR) {
      continue;
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
      break;
    }

    errno = saved_errno;
    ZNET_LOG_ERROR("TcpConnection::handle_read [{}] error: {}", name_,
                   strerror(errno));
    handle_error();
    return;
  }

  if (total > 0 && message_callback_) {
    // 直接在当前事件回调里执行，避免额外调度/锁竞争/动态分配。
    auto self = shared_from_this();
    message_callback_(self, &input_buffer_);
  }

  // 重新注册读事件，保证后续数据可继续驱动回调
  if (io_scheduler_ && connected()) {
    auto self = shared_from_this();
    io_scheduler_->add_event(socket_->fd(), zcoroutine::FdContext::kRead,
                             [self]() { self->handle_read(); });
  }
}

void TcpConnection::handle_write() {
  if (!connected()) {
    ZNET_LOG_WARN("TcpConnection::handle_write [{}] not connected", name_);
    return;
  }

  // 同 handle_read：写事件也会在 trigger_event 中被移除；并且使用 EPOLLET。
  // 因此这里必须尽可能写到 EAGAIN，若仍有剩余则重新注册 WRITE。
  int saved_errno = 0;
  bool wrote_any = false;

  while (true) {
    ssize_t n = 0;
    size_t remaining = 0;
    {
      std::lock_guard<zcoroutine::Spinlock> lock(output_buffer_lock_);
      if (output_buffer_.readable_bytes() == 0) {
        remaining = 0;
      } else {
        n = output_buffer_.write_fd(socket_->fd(), &saved_errno);
        remaining = output_buffer_.readable_bytes();
      }
    }

    if (remaining == 0) {
      // 数据全部发送完成，取消写事件
      if (io_scheduler_) {
        io_scheduler_->del_event(socket_->fd(), zcoroutine::FdContext::kWrite);
      }

      // 触发写完成回调（异步调度）
      if (wrote_any && write_complete_callback_ && io_scheduler_) {
        auto self = shared_from_this();
        auto cb = write_complete_callback_;
        if (zcoroutine::Scheduler::get_this() == io_scheduler_) {
          cb(self);
        } else {
          io_scheduler_->schedule(
              [self, cb = std::move(cb)]() mutable { cb(self); });
        }
      }

      if (state_.load(std::memory_order_acquire) == State::Disconnecting) {
        shutdown_in_loop();
      }
      return;
    }

    if (n > 0) {
      wrote_any = true;
      continue;
    }

    // n <= 0：要么 EAGAIN，要么错误
    if (saved_errno == EINTR) {
      continue;
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
      break;
    }

    errno = saved_errno;
    ZNET_LOG_ERROR("TcpConnection::handle_write [{}] error: {}", name_,
                   strerror(errno));
    handle_error();
    return;
  }

  // 仍有数据未发送完，重新注册写事件等待下次可写
  if (io_scheduler_ && connected()) {
    auto self = shared_from_this();
    io_scheduler_->add_event(socket_->fd(), zcoroutine::FdContext::kWrite,
                             [self]() { self->handle_write(); });
  }
}

void TcpConnection::handle_close() {
  // 避免重复关闭
  State current = state_.load(std::memory_order_acquire);
  if (current == State::Disconnected) {
    return;
  }

  ZNET_LOG_INFO("TcpConnection::handle_close [{}] state={}", name_,
                state_to_string(current));

  set_state(State::Disconnected);

  // 关闭 socket
  socket_->close();

  // 触发关闭回调（异步调度）
  if (close_callback_ && io_scheduler_) {
    auto self = shared_from_this();
    auto cb = close_callback_;
    if (zcoroutine::Scheduler::get_this() == io_scheduler_) {
      cb(self);
    } else {
      io_scheduler_->schedule([self, cb]() { cb(self); });
    }
  }
}

void TcpConnection::handle_error() {
  int err = socket_->get_error();
  ZNET_LOG_ERROR("TcpConnection::handle_error [{}] SO_ERROR={} {}", name_, err,
                 strerror(err));
  // 发生错误时关闭连接
  handle_close();
}

void TcpConnection::send_in_loop(const void *data, size_t len) {
  ssize_t n_wrote = 0;
  size_t remaining = len;
  bool fault_error = false;

  std::lock_guard<zcoroutine::Spinlock> lock(output_buffer_lock_);

  // 如果输出缓冲区没有数据，尝试直接发送
  if (output_buffer_.readable_bytes() == 0) {
    n_wrote = socket_->send(data, len);
    if (n_wrote >= 0) {
      remaining = len - n_wrote;
      if (remaining == 0 && write_complete_callback_ && io_scheduler_) {
        // 数据全部发送完成：若已在调度线程则直接调用，避免额外调度开销
        auto self = shared_from_this();
        auto cb = write_complete_callback_;
        if (zcoroutine::Scheduler::get_this() == io_scheduler_) {
          cb(self);
        } else {
          io_scheduler_->schedule([self, cb]() { cb(self); });
        }
      }
    } else {
      n_wrote = 0;
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        ZNET_LOG_ERROR("TcpConnection::send_in_loop [{}] error: {}", name_,
                       strerror(errno));
        if (errno == EPIPE || errno == ECONNRESET) {
          fault_error = true;
        }
      }
    }
  }

  // 如果还有数据未发送完，写入输出缓冲区
  if (!fault_error && remaining > 0) {
    output_buffer_.append(static_cast<const char *>(data) + n_wrote, remaining);

    // 注册写事件
    if (io_scheduler_) {
      auto self = shared_from_this();
      io_scheduler_->add_event(socket_->fd(), zcoroutine::FdContext::kWrite,
                               [self]() { self->handle_write(); });
    }

    ZNET_LOG_DEBUG("TcpConnection::send_in_loop [{}] buffered {} bytes, total "
                   "{} bytes in buffer",
                   name_, remaining, output_buffer_.readable_bytes());
  }
}

void TcpConnection::shutdown_in_loop() {
  std::lock_guard<zcoroutine::Spinlock> lock(output_buffer_lock_);
  if (output_buffer_.readable_bytes() == 0) {
    // 输出缓冲区为空，可以关闭写端
    socket_->shutdown_write();
  }
}

void TcpConnection::force_close_in_loop() {
  State current = state_.load(std::memory_order_acquire);
  if (current == State::Connected || current == State::Disconnecting) {
    handle_close();
  }
}

} // namespace znet
