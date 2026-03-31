#include "znet/tcp_connection.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "zcoroutine/sched.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {

// 仅用于日志与调试输出，帮助快速定位状态流转。
const char* state_to_string(TcpConnection::State state) {
  switch (state) {
    case TcpConnection::State::kDisconnected:
      return "disconnected";
    case TcpConnection::State::kConnecting:
      return "connecting";
    case TcpConnection::State::kConnected:
      return "connected";
    case TcpConnection::State::kDisconnecting:
      return "disconnecting";
  }
  return "unknown";
}

}  // namespace

// 构造阶段只做资源兜底与默认流初始化，不触发任何 I/O。
TcpConnection::TcpConnection(Socket::ptr socket,
                             Stream::ptr read_stream,
                             Stream::ptr write_stream)
    : socket_(std::move(socket)),
      read_stream_(std::move(read_stream)),
      write_stream_(std::move(write_stream)),
      state_(static_cast<uint8_t>(State::kConnecting)),
      owner_scheduler_(nullptr),
      owner_sched_id_(-1),
      write_complete_callback_(),
      high_water_mark_callback_(),
      high_water_mark_(64 * 1024 * 1024),
      context_(nullptr) {
  // 无效 socket 直接降级为断开态，避免后续调用误判为可用连接。
  if (!socket_ || !socket_->is_valid()) {
    ZNET_LOG_WARN("TcpConnection::TcpConnection created with invalid socket");
    state_.store(static_cast<uint8_t>(State::kDisconnected),
                 std::memory_order_release);
  }

  // 默认使用 BufferStream，满足“可缓存收发”这一最常见需求。
  if (!read_stream_) {
    read_stream_ = std::make_shared<BufferStream>();
  }
  if (!write_stream_) {
    write_stream_ = std::make_shared<BufferStream>();
  }

  ZNET_LOG_DEBUG("TcpConnection::TcpConnection initialized: fd={}, state={}",
                 fd(), state_to_string(state()));
}

// 当前待发送量由 write_stream 负责维护，连接对象仅做汇总访问。
size_t TcpConnection::pending_write_bytes() const {
  if (!write_stream_) {
    return 0;
  }
  return write_stream_->pending_bytes();
}

int TcpConnection::fd() const {
  if (!socket_) {
    return -1;
  }
  return socket_->fd();
}

// 绑定到当前调度器，建立“连接线程亲和性”。
void TcpConnection::bind_to_current_loop() {
  owner_sched_id_ = zcoroutine::sched_id();
  set_state(State::kConnected);
  ZNET_LOG_INFO("TcpConnection::bind_to_current_loop success: fd={}, sched_id={}",
                fd(), owner_sched_id_);
}

// 显式绑定指定调度器；空指针时回退到当前调度器绑定。
void TcpConnection::bind_to_loop(zcoroutine::Scheduler* scheduler) {
  owner_scheduler_ = scheduler;
  if (!owner_scheduler_) {
    bind_to_current_loop();
    return;
  }

  owner_sched_id_ = owner_scheduler_->id();
  set_state(State::kConnected);
  ZNET_LOG_INFO("TcpConnection::bind_to_loop success: fd={}, sched_id={}", fd(),
                owner_sched_id_);
}

// 将 Stream 与本连接互相关联，便于 Stream 访问 owner loop 与 socket。
void TcpConnection::bind_stream(const Stream::ptr& stream) {
  if (!stream) {
    return;
  }

  try {
    stream->set_connection(shared_from_this());
  } catch (const std::bad_weak_ptr&) {
    ZNET_LOG_ERROR(
        "TcpConnection::bind_stream failed to bind stream to connection due "
        "to bad_weak_ptr");
  }
}

// 设置读流并自动回填连接引用。
void TcpConnection::set_read_stream(Stream::ptr read_stream) {
  if (!read_stream) {
    read_stream = std::make_shared<BufferStream>();
  }
  read_stream_ = std::move(read_stream);
  bind_stream(read_stream_);
}

// 设置写流并自动回填连接引用。
void TcpConnection::set_write_stream(Stream::ptr write_stream) {
  if (!write_stream) {
    write_stream = std::make_shared<BufferStream>();
  }
  write_stream_ = std::move(write_stream);
  bind_stream(write_stream_);
}

void TcpConnection::set_streams(Stream::ptr read_stream,
                                Stream::ptr write_stream) {
  set_read_stream(std::move(read_stream));
  set_write_stream(std::move(write_stream));
}

// 原子状态写入并记录迁移日志，便于排查竞态或生命周期问题。
void TcpConnection::set_state(State state) {
  const State previous = this->state();
  state_.store(static_cast<uint8_t>(state), std::memory_order_release);
  if (previous != state) {
    ZNET_LOG_DEBUG("TcpConnection::set_state: fd={}, {} -> {}", fd(),
                   state_to_string(previous), state_to_string(state));
  }
}

// 仅当在协程上下文且调度器 id 一致时，视为 owner loop 内调用。
bool TcpConnection::in_owner_loop() const {
  if (owner_sched_id_ < 0) {
    return false;
  }
  return zcoroutine::in_coroutine() && zcoroutine::sched_id() == owner_sched_id_;
}

// 驱动读流执行一次“读入到流缓冲”操作。
ssize_t TcpConnection::read(size_t max_read_bytes, uint32_t timeout_ms) {
  const State current = state();
  if ((current != State::kConnected && current != State::kDisconnecting) ||
      !socket_ || !socket_->is_valid()) {
    errno = EBADF;
    ZNET_LOG_WARN("TcpConnection::read invalid state/socket: fd={}, state={}",
                  fd(), state_to_string(current));
    return -1;
  }

  if (max_read_bytes == 0) {
    errno = EINVAL;
    ZNET_LOG_WARN("TcpConnection::read max_read_bytes must be > 0");
    return -1;
  }

  if (!read_stream_) {
    errno = ENOTCONN;
    ZNET_LOG_WARN("TcpConnection::read read_stream is null: fd={}", fd());
    return -1;
  }

  const ssize_t n = read_stream_->read_to_buffer(max_read_bytes, timeout_ms);
  if (n > 0) {
    return n;
  }

  // n==0 视为对端正常关闭，状态转入 disconnected。
  if (n == 0) {
    set_state(State::kDisconnected);
    ZNET_LOG_INFO("TcpConnection::read peer closed: fd={}", fd());
  }
  return n;
}

// 刷新写流中的待发送数据，必要时触发写完成回调。
ssize_t TcpConnection::flush_output(uint32_t timeout_ms) {
  const State current = state();
  if ((current != State::kConnected && current != State::kDisconnecting) ||
      !socket_ || !socket_->is_valid()) {
    errno = EBADF;
    ZNET_LOG_WARN("TcpConnection::flush_output invalid state/socket: fd={}, state={}",
                  fd(), state_to_string(current));
    return -1;
  }

  // 保证 flush 与连接所属调度器一致，避免跨调度器并发写。
  if (owner_sched_id_ >= 0 && !in_owner_loop()) {
    errno = EOPNOTSUPP;
    ZNET_LOG_WARN("TcpConnection::flush_output called outside owner loop: fd={}, "
                  "owner_sched_id={}",
                  fd(), owner_sched_id_);
    return -1;
  }

  if (!write_stream_) {
    errno = ENOTCONN;
    ZNET_LOG_WARN("TcpConnection::flush_output write_stream is null: fd={}", fd());
    return -1;
  }

  const ssize_t stream_flushed = write_stream_->flush_buffer(timeout_ms);
  if (stream_flushed < 0) {
    ZNET_LOG_WARN("TcpConnection::flush_output stream flush failed: fd={}, "
                  "errno={}",
                  fd(), errno);
    return -1;
  }

  // 仅在“本次确实写出且写缓冲清空”时触发写完成回调。
  if (stream_flushed > 0 && pending_write_bytes() == 0 && write_complete_callback_) {
    write_complete_callback_(shared_from_this());
  }

  return stream_flushed;
}

// 外部发送入口：按 owner loop 亲和性决定“同步发送”或“投递发送任务”。
ssize_t TcpConnection::send(const void* data, size_t length, uint32_t timeout_ms) {
  if (!data && length > 0) {
    errno = EINVAL;
    ZNET_LOG_WARN("TcpConnection::send received null data with length={}", length);
    return -1;
  }

  if (length == 0) {
    return 0;
  }

  const State current = state();
  if (current != State::kConnected && current != State::kDisconnecting) {
    errno = EBADF;
    ZNET_LOG_WARN("TcpConnection::send invalid state: fd={}, state={}", fd(),
                  state_to_string(current));
    return -1;
  }

  // 非 owner loop 调用时，转投递到 owner_scheduler_ 异步执行。
  if (owner_sched_id_ >= 0 && !in_owner_loop()) {
    if (!owner_scheduler_) {
      errno = EOPNOTSUPP;
      ZNET_LOG_ERROR("TcpConnection::send owner loop mismatch without scheduler: "
                     "fd={}, owner_sched_id={}",
                     fd(), owner_sched_id_);
      return -1;
    }

    std::shared_ptr<TcpConnection> self = shared_from_this();
    // 复制 payload 确保跨协程调度后数据仍然有效。
    std::string payload(static_cast<const char*>(data), length);
    ZNET_LOG_DEBUG("TcpConnection::send dispatch to owner loop: fd={}, bytes={}, "
                   "owner_sched_id={}",
                   fd(), length, owner_sched_id_);
    owner_scheduler_->go([self, payload, timeout_ms]() {
      const State state = self->state();
      if (state != State::kConnected && state != State::kDisconnecting) {
        return;
      }
      (void)self->send_in_loop(payload.data(), payload.size(), timeout_ms);
    });
    return static_cast<ssize_t>(length);
  }

  return send_in_loop(data, length, timeout_ms);
}

// owner loop 内的实际发送路径：先写入 write_stream，再 flush 到 socket。
ssize_t TcpConnection::send_in_loop(const void* data, size_t length,
                                    uint32_t timeout_ms) {
  const State current = state();
  if (current != State::kConnected && current != State::kDisconnecting) {
    errno = EBADF;
    ZNET_LOG_WARN("TcpConnection::send_in_loop invalid state: fd={}, state={}",
                  fd(), state_to_string(current));
    return -1;
  }

  if (!write_stream_) {
    errno = ENOTCONN;
    ZNET_LOG_WARN("TcpConnection::send_in_loop write_stream is null: fd={}", fd());
    return -1;
  }

  const size_t old_pending = pending_write_bytes();

  size_t written = 0;
  // write_stream::write 可能分段接受数据，因此循环直到全部写入流。
  while (written < length) {
    const ssize_t n = write_stream_->write(
        static_cast<const char*>(data) + written, length - written, timeout_ms);
    if (n <= 0) {
      if (n == 0) {
        errno = EIO;
      }
      ZNET_LOG_WARN("TcpConnection::send_in_loop write_stream write failed: fd={}, "
                    "errno={}",
                    fd(), errno);
      return -1;
    }
    written += static_cast<size_t>(n);
  }

  const size_t new_pending = pending_write_bytes();
  // 仅在跨越阈值瞬间触发一次高水位事件，避免重复告警。
  if (high_water_mark_callback_ && old_pending < high_water_mark_ &&
      new_pending >= high_water_mark_) {
    high_water_mark_callback_(shared_from_this(), new_pending);
  }

  const ssize_t flushed = flush_output(timeout_ms);
  if (flushed < 0) {
    ZNET_LOG_WARN("TcpConnection::send_in_loop flush_output failed: fd={}, errno={}",
                  fd(), errno);
    return -1;
  }

  // 若已进入优雅关闭阶段，发送完成后立即执行真正 close。
  if (state() == State::kDisconnecting) {
    close();
  }

  return static_cast<ssize_t>(length);
}

// 优雅关闭：先停收新写入，再尝试把已有缓存数据发送完毕。
void TcpConnection::shutdown() {
  const State current = state();
  if (current != State::kConnected) {
    ZNET_LOG_DEBUG("TcpConnection::shutdown skipped: fd={}, state={}", fd(),
                   state_to_string(current));
    return;
  }

  set_state(State::kDisconnecting);
  ZNET_LOG_INFO("TcpConnection::shutdown begin: fd={}", fd());

  // 非 owner loop 触发时，将“flush+close”投递到 owner loop 执行。
  if (owner_sched_id_ >= 0 && !in_owner_loop() && owner_scheduler_) {
    std::shared_ptr<TcpConnection> self = shared_from_this();
    owner_scheduler_->go([self]() {
      if (self->state() == State::kDisconnecting) {
        (void)self->flush_output();
        self->close();
      }
    });
    return;
  }

  (void)flush_output();
  close();
}

// 立即关闭：幂等地转入 disconnected 并关闭底层 fd。
void TcpConnection::close() {
  State expected = state();
  if (expected == State::kDisconnected) {
    ZNET_LOG_DEBUG("TcpConnection::close skipped: already disconnected fd={}",
                   fd());
    return;
  }
  set_state(State::kDisconnected);

  if (socket_) {
    (void)socket_->close();
  }

  ZNET_LOG_INFO("TcpConnection::close completed: fd={}", fd());
}

}  // namespace znet
