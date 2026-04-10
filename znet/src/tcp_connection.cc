#include "znet/tcp_connection.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>
#include <vector>

#include "zcoroutine/io_event.h"
#include "zcoroutine/sched.h"
#include "znet/tls_context.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {

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

TcpConnection::TcpConnection(Socket::ptr socket,
                             zcoroutine::Scheduler* actor_scheduler)
    : socket_(std::move(socket)),
      input_buffer_(),
      output_buffer_(),
      state_(static_cast<uint8_t>(State::kConnecting)),
      write_complete_callback_(),
      high_water_mark_callback_(),
      high_water_mark_(64 * 1024 * 1024),
      write_timeout_ms_(0),
      tls_channel_(nullptr),
      context_(nullptr),
      actor_mutex_(),
      mailbox_(),
      actor_running_(false),
      actor_thread_id_(),
      actor_scheduler_(actor_scheduler),
      actor_sched_id_(-1) {
  if (actor_scheduler_) {
    actor_sched_id_ = actor_scheduler_->id();
  } else {
    actor_scheduler_ = zcoroutine::next_sched();
    if (!actor_scheduler_) {
      actor_scheduler_ = zcoroutine::main_sched();
    }
    if (actor_scheduler_) {
      actor_sched_id_ = actor_scheduler_->id();
    }
  }

  if (!socket_ || !socket_->is_valid()) {
    ZNET_LOG_WARN("TcpConnection::TcpConnection created with invalid socket");
    state_.store(static_cast<uint8_t>(State::kDisconnected),
                 std::memory_order_release);
  } else {
    state_.store(static_cast<uint8_t>(State::kConnected),
                 std::memory_order_release);
  }

  ZNET_LOG_DEBUG(
      "TcpConnection::TcpConnection initialized: fd={}, state={}, actor_sched_id={}",
      fd(), state_to_string(state()), actor_sched_id_);
}

  TcpConnection::~TcpConnection() = default;

size_t TcpConnection::pending_write_bytes() const {
  return output_buffer_.readable_bytes();
}

uint32_t TcpConnection::resolve_write_timeout(uint32_t timeout_ms) const {
  if (timeout_ms == kUseConnectionWriteTimeout) {
    return write_timeout();
  }
  return timeout_ms;
}

int TcpConnection::fd() const {
  if (!socket_) {
    return -1;
  }
  return socket_->fd();
}

void TcpConnection::set_state(State state) {
  const State previous = this->state();
  state_.store(static_cast<uint8_t>(state), std::memory_order_release);
  if (previous != state) {
    ZNET_LOG_DEBUG("TcpConnection::set_state: fd={}, {} -> {}", fd(),
                   state_to_string(previous), state_to_string(state));
  }
}

// 事件调度逻辑：
// 1. 事件通过 dispatch_event_and_wait 投递到连接内部，等待处理完成。事件类型包括读、写、关闭等。
// 2. 连接内部维护一个 Actor 邮箱，串行处理事件，避免并发访问冲突。处理完成后通过条件变量通知等待的调用方。
// 3. 事件处理函数根据事件类型执行对应的读写/关闭逻辑，更新连接状态，并设置事件结果和错误码。
ssize_t TcpConnection::dispatch_event_and_wait(const std::shared_ptr<Event>& event) {
  if (!event) {
    errno = EINVAL;
    return -1;
  }

  // 当actor线程首次开启启动时，分发协程来处理事件，
  // 后续事件由actor线程自己处理，无需每次分发都启动新协程。
  bool should_launch_worker = false;
  
  // 当前是协程上下文且调度器匹配时，直接在当前协程处理事件，避免不必要的协程切换和调度延迟。
  // 一般是外部使用协程调用 TcpConnection 的公共接口（如 send）时，事件会直接在当前协程处理。
  // 以次避免在协程上下文中再创建协程处理事件导致的性能损耗和复杂度增加。
  bool run_inline = false; 
  
  // 是否是 Actor 内部调用（事件处理函数内部再次调用 dispatch_event_and_wait），
  // 比如 flush 内部调用 send，此时直接处理事件避免死锁。
  bool reentrant = false;  
  {
    std::unique_lock<std::mutex> lock(actor_mutex_);
    if (actor_running_ && actor_thread_id_ == std::this_thread::get_id()) {
      reentrant = true;
    } else {
      mailbox_.push_back(event);
      if (!actor_running_) {
        actor_running_ = true;
        if (zcoroutine::in_coroutine() &&
            (actor_sched_id_ < 0 || zcoroutine::sched_id() == actor_sched_id_)) {
          run_inline = true;
        } else {
          should_launch_worker = true;
        }
      }
    }
  }

  if (reentrant) {
    process_event(event);
    if (event->error != 0) {
      errno = event->error;
    }
    return event->result;
  }

  if (run_inline) {
    drain_mailbox();
  } else if (should_launch_worker) {
    std::shared_ptr<TcpConnection> self = shared_from_this();
    if (actor_scheduler_) {
      actor_scheduler_->go([self]() { self->drain_mailbox(); });
    } else {
      zcoroutine::go([self]() { self->drain_mailbox(); });
    }
  }

  event->completion.wait();

  if (event->error != 0) {
    errno = event->error;
  }
  return event->result;
}

bool TcpConnection::try_begin_inline_actor() {
  if (!zcoroutine::in_coroutine()) {
    return false;
  }

  if (actor_sched_id_ >= 0 && zcoroutine::sched_id() != actor_sched_id_) {
    return false;
  }

  std::unique_lock<std::mutex> lock(actor_mutex_);
  if (actor_running_) {
    return false;
  }

  actor_running_ = true;
  actor_thread_id_ = std::this_thread::get_id();
  return true;
}

void TcpConnection::finish_inline_actor() {
  drain_mailbox();
}

void TcpConnection::drain_mailbox() {
  {
    std::unique_lock<std::mutex> lock(actor_mutex_);
    actor_thread_id_ = std::this_thread::get_id();
  }

  while (true) {
    std::shared_ptr<Event> event;
    {
      std::unique_lock<std::mutex> lock(actor_mutex_);
      if (mailbox_.empty()) {
        actor_running_ = false;
        actor_thread_id_ = std::thread::id();
        return;
      }
      event = mailbox_.front();
      mailbox_.pop_front();
    }

    process_event(event);
    event->completion.done();
  }
}

void TcpConnection::process_event(const std::shared_ptr<Event>& event) {
  errno = 0;

  switch (event->type) {
    case EventType::kRead:
      event->result = read_internal(event->max_read_bytes, event->timeout_ms);
      break;
    case EventType::kSend:
      event->result =
          send_internal(event->payload.data(), event->payload.size(), event->timeout_ms);
      break;
    case EventType::kFlush:
      event->result = flush_output_internal(event->timeout_ms);
      break;
    case EventType::kShutdown:
      shutdown_internal();
      event->result = 0;
      break;
    case EventType::kClose:
      close_internal();
      event->result = 0;
      break;
  }

  event->error = (event->result < 0) ? errno : 0;
}

ssize_t TcpConnection::read_internal(size_t max_read_bytes, uint32_t timeout_ms) {
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

  if (tls_channel_) {
    return read_tls_internal(max_read_bytes, timeout_ms);
  }

  int saved_errno = 0;
    const ssize_t n = input_buffer_.read_from_socket(
      socket_, max_read_bytes, timeout_ms, &saved_errno);
  if (n < 0) {
    errno = saved_errno;
    return -1;
  }

  if (n == 0) {
    set_state(State::kDisconnected);
    ZNET_LOG_INFO("TcpConnection::read peer closed: fd={}", fd());
  }

  return n;
}

bool TcpConnection::enable_tls_server(const std::shared_ptr<TlsContext>& tls_context,
                                      uint32_t handshake_timeout_ms) {
  if (!tls_context || !socket_ || !socket_->is_valid()) {
    errno = EINVAL;
    return false;
  }

  if (tls_channel_) {
    return true;
  }

  auto channel = tls_context->create_server_channel(fd());
  if (!channel) {
    ZNET_LOG_ERROR(
        "TcpConnection::enable_tls_server create_server_channel failed: fd={}, errno={}",
        fd(), errno);
    return false;
  }

  if (!channel->handshake(
          handshake_timeout_ms,
          [this](bool wait_for_write, uint32_t timeout_ms) {
            return wait_tls_io(wait_for_write, timeout_ms);
          })) {
    ZNET_LOG_WARN(
        "TcpConnection::enable_tls_server handshake failed: fd={}, errno={}",
        fd(), errno);
    return false;
  }

  tls_channel_ = std::move(channel);
  ZNET_LOG_INFO("TcpConnection::enable_tls_server handshake success: fd={}", fd());
  return true;
}

bool TcpConnection::wait_tls_io(bool wait_for_write, uint32_t timeout_ms) {
  zcoroutine::IoEvent io_event(
      fd(), wait_for_write ? zcoroutine::IoEventType::kWrite
                           : zcoroutine::IoEventType::kRead);
  const uint32_t wait_timeout_ms =
      timeout_ms == 0 ? zcoroutine::kInfiniteTimeoutMs : timeout_ms;
  if (!io_event.wait(wait_timeout_ms)) {
    if (errno == 0 && zcoroutine::timeout()) {
      errno = ETIMEDOUT;
    }
    return false;
  }
  return true;
}

ssize_t TcpConnection::read_tls_internal(size_t max_read_bytes,
                                         uint32_t timeout_ms) {
  if (!tls_channel_) {
    errno = EINVAL;
    return -1;
  }

  const size_t max_chunk = std::min(
      max_read_bytes, static_cast<size_t>(std::numeric_limits<int>::max()));
  std::vector<char> read_buffer(max_chunk);

  const ssize_t n = tls_channel_->read(
      read_buffer.data(), read_buffer.size(), timeout_ms,
      [this](bool wait_for_write, uint32_t wait_timeout_ms) {
        return wait_tls_io(wait_for_write, wait_timeout_ms);
      });

  if (n > 0) {
    input_buffer_.append(read_buffer.data(), static_cast<size_t>(n));
    return n;
  }

  if (n == 0) {
    set_state(State::kDisconnected);
  }

  return n;
}

ssize_t TcpConnection::flush_output_internal(uint32_t timeout_ms) {
  const State current = state();
  if ((current != State::kConnected && current != State::kDisconnecting) ||
      !socket_ || !socket_->is_valid()) {
    errno = EBADF;
    ZNET_LOG_WARN(
        "TcpConnection::flush_output invalid state/socket: fd={}, state={}",
        fd(), state_to_string(current));
    return -1;
  }

  ssize_t sent_total = 0;
  if (tls_channel_) {
    while (output_buffer_.readable_bytes() > 0) {
      const char* data = output_buffer_.peek();
      const size_t bytes = output_buffer_.readable_bytes();
      const ssize_t n = write_tls_internal(data, bytes, timeout_ms);
      if (n <= 0) {
        if (n < 0) {
          ZNET_LOG_WARN("TcpConnection::flush_output tls write failed: fd={}, errno={}",
                        fd(), errno);
          return -1;
        }
        break;
      }

      output_buffer_.retrieve(static_cast<size_t>(n));
      sent_total += n;
    }

    if (sent_total > 0 && output_buffer_.readable_bytes() == 0 &&
        write_complete_callback_) {
      write_complete_callback_(shared_from_this());
    }

    return sent_total;
  }

  while (output_buffer_.readable_bytes() > 0) {
    int saved_errno = 0;
    const ssize_t n =
        output_buffer_.write_to_socket(socket_, timeout_ms, &saved_errno);
    if (n > 0) {
      sent_total += n;
      continue;
    }

    if (n == 0) {
      break;
    }

    if (saved_errno == EINTR) {
      continue;
    }

    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
      break;
    }

    errno = saved_errno;
    ZNET_LOG_WARN("TcpConnection::flush_output socket write failed: fd={}, errno={}",
                  fd(), errno);
    return -1;
  }

  if (sent_total > 0 && output_buffer_.readable_bytes() == 0 &&
      write_complete_callback_) {
    write_complete_callback_(shared_from_this());
  }

  return sent_total;
}

ssize_t TcpConnection::write_tls_internal(const char* data,
                                          size_t length,
                                          uint32_t timeout_ms) {
  if (!tls_channel_ || !data || length == 0) {
    errno = EINVAL;
    return -1;
  }

  return tls_channel_->write(
      data, length, timeout_ms,
      [this](bool wait_for_write, uint32_t wait_timeout_ms) {
        return wait_tls_io(wait_for_write, wait_timeout_ms);
      });
}

void TcpConnection::shutdown_tls_internal() {
  if (!tls_channel_) {
    return;
  }

  tls_channel_->shutdown(
      1000, [this](bool wait_for_write, uint32_t wait_timeout_ms) {
        return wait_tls_io(wait_for_write, wait_timeout_ms);
      });
}

void TcpConnection::close_tls_internal() {
  if (!tls_channel_) {
    return;
  }

  tls_channel_.reset();
}

ssize_t TcpConnection::send_internal(const char* data,
                                     size_t length,
                                     uint32_t timeout_ms) {
  const State current = state();
  if (current != State::kConnected && current != State::kDisconnecting) {
    errno = EBADF;
    ZNET_LOG_WARN("TcpConnection::send invalid state: fd={}, state={}", fd(),
                  state_to_string(current));
    return -1;
  }

  if (!data && length > 0) {
    errno = EINVAL;
    return -1;
  }

  if (length == 0) {
    return 0;
  }

  const size_t old_pending = pending_write_bytes();
  const size_t projected_pending = old_pending + length;
  if (high_water_mark_callback_ && old_pending < high_water_mark_ &&
      projected_pending >= high_water_mark_) {
    high_water_mark_callback_(shared_from_this(), projected_pending);
  }

  size_t buffered_offset = 0;
  if (!tls_channel_ && old_pending == 0) {
    const ssize_t direct_sent = socket_->send(data, length, 0, timeout_ms);
    if (direct_sent == static_cast<ssize_t>(length)) {
      if (write_complete_callback_) {
        write_complete_callback_(shared_from_this());
      }
      if (state() == State::kDisconnecting) {
        close_internal();
      }
      return static_cast<ssize_t>(length);
    }

    if (direct_sent > 0) {
      buffered_offset = static_cast<size_t>(direct_sent);
    } else if (direct_sent < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
      return -1;
    }
  }

  if (buffered_offset < length) {
    output_buffer_.append(data + buffered_offset, length - buffered_offset);
  }

  const ssize_t flushed = flush_output_internal(timeout_ms);
  if (flushed < 0) {
    return -1;
  }

  if (state() == State::kDisconnecting && output_buffer_.readable_bytes() == 0) {
    close_internal();
  }

  return static_cast<ssize_t>(length);
}

void TcpConnection::shutdown_internal() {
  const State current = state();
  if (current == State::kDisconnected) {
    return;
  }

  if (current == State::kConnected) {
    set_state(State::kDisconnecting);
  }

  (void)flush_output_internal(0);
  shutdown_tls_internal();
  close_internal();
}

void TcpConnection::close_internal() {
  const State current = state();
  if (current == State::kDisconnected) {
    return;
  }

  set_state(State::kDisconnected);
  close_tls_internal();
  if (socket_) {
    (void)socket_->close();
  }

  ZNET_LOG_INFO("TcpConnection::close completed: fd={}", fd());
}

ssize_t TcpConnection::read(size_t max_read_bytes, uint32_t timeout_ms) {
  if (try_begin_inline_actor()) {
    const ssize_t result = read_internal(max_read_bytes, timeout_ms);
    const int saved_errno = errno;
    finish_inline_actor();
    errno = saved_errno;
    return result;
  }

  std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kRead);
  event->max_read_bytes = max_read_bytes;
  event->timeout_ms = timeout_ms;
  return dispatch_event_and_wait(event);
}

ssize_t TcpConnection::flush_output(uint32_t timeout_ms) {
  const uint32_t effective_timeout_ms = resolve_write_timeout(timeout_ms);
  if (try_begin_inline_actor()) {
    const ssize_t result = flush_output_internal(effective_timeout_ms);
    const int saved_errno = errno;
    finish_inline_actor();
    errno = saved_errno;
    return result;
  }

  std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kFlush);
  event->timeout_ms = effective_timeout_ms;
  return dispatch_event_and_wait(event);
}

ssize_t TcpConnection::send(const void* data, size_t length,
                            uint32_t timeout_ms) {
  if (!data && length > 0) {
    errno = EINVAL;
    ZNET_LOG_WARN("TcpConnection::send received null data with length={}",
                  length);
    return -1;
  }

  if (length == 0) {
    return 0;
  }

  const uint32_t effective_timeout_ms = resolve_write_timeout(timeout_ms);
  if (try_begin_inline_actor()) {
    const ssize_t result =
        send_internal(static_cast<const char*>(data), length, effective_timeout_ms);
    const int saved_errno = errno;
    finish_inline_actor();
    errno = saved_errno;
    return result;
  }

  std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kSend);
  event->timeout_ms = effective_timeout_ms;
  event->payload.assign(static_cast<const char*>(data), length);
  return dispatch_event_and_wait(event);
}

void TcpConnection::shutdown() {
  if (try_begin_inline_actor()) {
    shutdown_internal();
    const int saved_errno = errno;
    finish_inline_actor();
    errno = saved_errno;
    return;
  }

  std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kShutdown);
  (void)dispatch_event_and_wait(event);
}

void TcpConnection::close() {
  if (try_begin_inline_actor()) {
    close_internal();
    const int saved_errno = errno;
    finish_inline_actor();
    errno = saved_errno;
    return;
  }

  std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kClose);
  (void)dispatch_event_and_wait(event);
}

}  // namespace znet
