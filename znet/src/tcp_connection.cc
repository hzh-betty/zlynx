#include "tcp_connection.h"
#include "io_scheduler.h"
#include "znet_logger.h"
#include <errno.h>
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

  // ET 读循环里，EAGAIN 本来应该是“这一轮读完了，赶紧退出并重新注册读事件（re-arm）”。
  // 但 hook 把它改成了“当前协程挂起，等下次可读再回来”。里面也有个 while，
  // 可能导致add_event不及时，错过数据到达的时机，导致连接上数据长期滞留在内核缓冲区里。
  // 因此这里必须设置非阻塞，确保 ET 模式下的正确行为。
  socket_->set_non_blocking(true);

  // 注册读事件到IoScheduler
  // 这里只注册“首次读事件”。后续每次 handle_read() 处理完一轮事件后，
  // 都会手动 re-arm 下一次读事件。这是当前 ET 模式下的固定套路。
  if (io_scheduler_) {
    auto self = shared_from_this();
    io_scheduler_->add_event(socket_->fd(), zcoroutine::Channel::kRead,
                             [self]() { self->handle_read(); });
  }

  // 触发连接建立回调（异步调度）
  if (connection_callback_) {
    auto self = shared_from_this();
    auto cb = connection_callback_;
    if (io_scheduler_) {
      io_scheduler_->schedule(
          [self, cb = std::move(cb)]() mutable { cb(self); });
    } else {
      ZNET_LOG_FATAL("TcpConnection::connect_established no IoScheduler for "
                     "connection callback");
    }
  }

  ZNET_LOG_INFO("TcpConnection::connect_established [{}] fd={}", name_,
                socket_->fd());

  // 连接刚建立时，还没有任何请求字节进来。
  // 因此 read_timeout 的第一段语义是：客户端在建连后多久必须开始说话。
  refresh_read_timer();

  // EPOLLET 下存在一个经典竞态：客户端可能在服务端把新连接 fd 注册进 epoll
  // 之前就已经发送了首包数据。
  // 这种情况下，如果我们只依赖“下一次边沿触发”，可能永远等不到 READ 事件，
  // 导致连接上的数据长期滞留在内核缓冲区里。
  // 因此在连接建立完成后主动尝试读一轮（非阻塞读到 EAGAIN 即停），把
  // 已经到达的数据及时交给上层协议解析。
  handle_read();
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

void TcpConnection::set_read_timeout(uint64_t timeout_ms) {
  read_timeout_ms_ = timeout_ms;
  if (timeout_ms == 0) {
    cancel_read_timer();
  }
}

void TcpConnection::set_write_timeout(uint64_t timeout_ms) {
  write_timeout_ms_ = timeout_ms;
  if (timeout_ms == 0) {
    cancel_write_timer();
  }
}

void TcpConnection::set_keepalive_timeout(uint64_t timeout_ms) {
  keepalive_timeout_ms_ = timeout_ms;
  if (timeout_ms == 0) {
    cancel_keepalive_timer();
  }
}

void TcpConnection::finish_response(bool keep_alive) {
  // finish_response() 是协议层对连接层的一个“状态切换通知”：
  // 当前这一轮请求已经处理完，连接接下来要么关闭，要么进入 keep-alive 空闲期。
  // 这里不负责真正 send 响应数据，send 已经先做了；这里负责的是 timeout
  // 状态机切换。

  // 一轮请求已经完成，因此本次请求对应的 read_timeout 计时结束。
  cancel_read_timer();

  {
    std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
    keepalive_waiting_ = keep_alive;
  }

  if (!keep_alive) {
    // 响应声明不再保持连接，则也不需要 keepalive 空闲计时器。
    cancel_keepalive_timer();
    return;
  }

  // keep_alive=true 时，并不是立即“重新进入 read timeout”，而是先进入
  // keepalive 空闲期：等待下一个请求到达；若一直没有新请求，则按
  // keepalive_timeout 主动回收连接。
  arm_keepalive_timer_if_needed();
}

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
    if (saved_errno == ECONNRESET || saved_errno == ECONNABORTED) {
      ZNET_LOG_DEBUG("TcpConnection::handle_read [{}] peer reset, errno={} {}",
                     name_, saved_errno, strerror(saved_errno));
      handle_close();
      return;
    }

    errno = saved_errno;
    ZNET_LOG_ERROR("TcpConnection::handle_read [{}] error: {}", name_,
                   strerror(errno));
    handle_error();
    return;
  }

  if (total > 0 && message_callback_) {
    // 一旦读到了新数据，说明连接已经离开 keep-alive idle 状态。
    // 因此要先取消 keepalive timer，并把状态切回“正在处理请求”。
    cancel_keepalive_timer();
    {
      std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
      keepalive_waiting_ = false;
    }

    // 读取阶段有进展，重新刷新 read timeout。
    refresh_read_timer();

    // 直接在当前事件回调里执行，避免额外调度/锁竞争/动态分配。
    auto self = shared_from_this();
    message_callback_(self, &input_buffer_);
  }

  // 重新注册读事件，保证后续数据可继续驱动回调
  if (io_scheduler_ && connected()) {
    auto self = shared_from_this();
    io_scheduler_->add_event(socket_->fd(), zcoroutine::Channel::kRead,
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
      // 输出缓冲区已经刷空，说明本轮 write timeout 风险解除。
      cancel_write_timer();

      // 数据全部发送完成，取消写事件
      if (io_scheduler_) {
        io_scheduler_->del_event(socket_->fd(), zcoroutine::Channel::kWrite);
      }

      // 触发写完成回调（异步调度）
      if (wrote_any && write_complete_callback_ && io_scheduler_) {
        auto self = shared_from_this();
        auto cb = write_complete_callback_;
        io_scheduler_->schedule(
            [self, cb = std::move(cb)]() mutable { cb(self); });
      }

      if (state_.load(std::memory_order_acquire) == State::Disconnecting) {
        shutdown_in_loop();
      } else {
        // 响应真正发完后，连接才可能进入 keep-alive idle 状态。
        // 如果缓冲区还有待写数据，就不能算“空闲”。
        arm_keepalive_timer_if_needed();
      }
      return;
    }

    if (n > 0) {
      wrote_any = true;
      // 有写进展就继续续命 write timer。
      // write_timeout 的语义不是“整个响应总耗时”，而是“输出缓冲区长时间
      // 没有刷空/没有进展”。
      arm_write_timer();
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
    io_scheduler_->add_event(socket_->fd(), zcoroutine::Channel::kWrite,
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
  cancel_read_timer();
  cancel_write_timer();
  cancel_keepalive_timer();

  // 关闭 socket
  socket_->close();

  // 触发关闭回调（异步调度）
  if (close_callback_ && io_scheduler_) {
    auto self = shared_from_this();
    auto cb = close_callback_;
    io_scheduler_->schedule([self, cb]() { cb(self); });
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
        io_scheduler_->schedule([self, cb]() { cb(self); });
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
    // 进入 output_buffer 说明这次 send 已经从“同步立即写完”退化成
    // “等待后续可写事件慢慢刷出”。从这里开始 write timeout 才有意义。
    output_buffer_.append(static_cast<const char *>(data) + n_wrote, remaining);
    arm_write_timer();

    // 注册写事件
    if (io_scheduler_) {
      auto self = shared_from_this();
      io_scheduler_->add_event(socket_->fd(), zcoroutine::Channel::kWrite,
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

void TcpConnection::refresh_read_timer() {
  if (!io_scheduler_ || read_timeout_ms_ == 0 || !connected()) {
    return;
  }

  // 连接层的 timer 统一挂在所属 io_scheduler_ 上，这样回调会回到正确的
  // 事件线程里执行，不需要额外跨线程同步。
  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (read_timer_) {
    // 已存在 timer 时直接 reset，相当于“读取还有进展，延后超时点”。
    read_timer_->reset(read_timeout_ms_);
    return;
  }

  std::weak_ptr<TcpConnection> weak_self = shared_from_this();
  read_timer_ = io_scheduler_->add_timer(read_timeout_ms_, [weak_self]() {
    if (auto self = weak_self.lock()) {
      self->close_for_timeout("read");
    }
  });
}

void TcpConnection::arm_write_timer() {
  if (!io_scheduler_ || write_timeout_ms_ == 0 || !connected()) {
    return;
  }

  // write timeout 只在“已经存在待发送输出数据”时才会被 arm。
  // 如果一个响应可以一次 send 完，根本不会走到这里。
  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (write_timer_) {
    write_timer_->reset(write_timeout_ms_);
    return;
  }

  std::weak_ptr<TcpConnection> weak_self = shared_from_this();
  write_timer_ = io_scheduler_->add_timer(write_timeout_ms_, [weak_self]() {
    if (auto self = weak_self.lock()) {
      self->close_for_timeout("write");
    }
  });
}

void TcpConnection::cancel_read_timer() {
  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (!read_timer_) {
    return;
  }
  read_timer_->cancel();
  read_timer_.reset();
}

void TcpConnection::cancel_write_timer() {
  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (!write_timer_) {
    return;
  }
  write_timer_->cancel();
  write_timer_.reset();
}

void TcpConnection::cancel_keepalive_timer() {
  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (!keepalive_timer_) {
    return;
  }
  keepalive_timer_->cancel();
  keepalive_timer_.reset();
}

void TcpConnection::arm_keepalive_timer_if_needed() {
  if (!io_scheduler_ || keepalive_timeout_ms_ == 0 || !connected()) {
    return;
  }

  {
    std::lock_guard<zcoroutine::Spinlock> buffer_lock(output_buffer_lock_);
    if (output_buffer_.readable_bytes() > 0) {
      // 仍有待发送数据时，连接处于“写出中”而不是“keep-alive idle”。
      return;
    }
  }

  std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
  if (!keepalive_waiting_) {
    // 只有协议层明确把连接切到 keep-alive waiting 状态，这个计时器才允许启动。
    return;
  }

  if (keepalive_timer_) {
    keepalive_timer_->reset(keepalive_timeout_ms_);
    return;
  }

  std::weak_ptr<TcpConnection> weak_self = shared_from_this();
  keepalive_timer_ =
      io_scheduler_->add_timer(keepalive_timeout_ms_, [weak_self]() {
        if (auto self = weak_self.lock()) {
          self->close_for_timeout("keepalive");
        }
      });
}

void TcpConnection::close_for_timeout(const char *reason) {
  if (!connected()) {
    return;
  }

  // timeout 回调真正执行时，连接状态可能已经发生变化。
  // 因此这里要做“二次确认”，避免 timer 触发瞬间误关一个其实已经恢复正常的连接。
  bool should_close = true;
  if (strcmp(reason, "write") == 0) {
    std::lock_guard<zcoroutine::Spinlock> buffer_lock(output_buffer_lock_);
    if (output_buffer_.readable_bytes() == 0) {
      // 写缓冲区已经清空，说明 write timeout 条件已经自然消失。
      return;
    }
  } else if (strcmp(reason, "keepalive") == 0) {
    std::lock_guard<zcoroutine::Spinlock> lock(timeout_lock_);
    if (!keepalive_waiting_) {
      // 连接已经重新进入请求处理态，不应该再按 keepalive timeout 关闭。
      should_close = false;
    }
  }

  if (!should_close) {
    return;
  }

  ZNET_LOG_WARN("TcpConnection [{}] closed by {} timeout", name_, reason);
  force_close();
}

} // namespace znet
