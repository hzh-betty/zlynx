#include "znet/tcp_connection.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>
#include <vector>

#include "zco/io_event.h"
#include "zco/sched.h"

#include "znet/tls_context.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {

const char *state_to_string(TcpConnection::State state) {
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

} // namespace

TcpConnection::TcpConnection(Socket::ptr socket,
                             zco::Scheduler *actor_scheduler)
    : socket_(std::move(socket)), input_buffer_(), output_buffer_(),
      state_(static_cast<uint8_t>(State::kConnecting)),
      write_complete_callback_(), high_water_mark_callback_(),
      high_water_mark_(64 * 1024 * 1024), write_timeout_ms_(0),
      tls_channel_(nullptr), context_(nullptr), actor_mutex_(), mailbox_(),
      actor_running_(false), actor_thread_id_(),
      actor_scheduler_(actor_scheduler), actor_sched_id_(-1) {
    if (actor_scheduler_) {
        actor_sched_id_ = actor_scheduler_->id();
    } else {
        // 未显式指定时自动选择调度器，优先 next_sched，兜底 main_sched。
        actor_scheduler_ = zco::next_sched();
        if (!actor_scheduler_) {
            actor_scheduler_ = zco::main_sched();
        }
        if (actor_scheduler_) {
            actor_sched_id_ = actor_scheduler_->id();
        }
    }

    if (!socket_ || !socket_->is_valid()) {
        ZNET_LOG_WARN(
            "TcpConnection::TcpConnection created with invalid socket");
        state_.store(static_cast<uint8_t>(State::kDisconnected),
                     std::memory_order_release);
    } else {
        state_.store(static_cast<uint8_t>(State::kConnected),
                     std::memory_order_release);
    }

    ZNET_LOG_DEBUG("TcpConnection::TcpConnection initialized: fd={}, state={}, "
                   "actor_sched_id={}",
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

// 连接内部采用 Actor 模型串行化所有读写/关闭事件：
// 1) 外部线程/协程通过 dispatch_event_and_wait 投递 Event。
// 2) 事件进入 mailbox_，由单个执行者 drain_mailbox() 依次处理。
// 3) 处理结果写回 Event(result/error)，唤醒等待方。
// 该模型的核心目标是：避免同一连接上并发读写导致状态竞争。
ssize_t
TcpConnection::dispatch_event_and_wait(const std::shared_ptr<Event> &event) {
    if (!event) {
        errno = EINVAL;
        return -1;
    }

    // actor 尚未运行时，需要决定谁来启动 drain_mailbox。
    bool should_launch_worker = false;

    // 当前就在目标协程调度器上时，可直接 inline 处理，
    // 避免“再派发一个协程”带来的额外调度开销。
    bool run_inline = false;

    // 重入场景：若当前已在 actor 执行线程内，再次 dispatch 时必须直接执行，
    // 否则会出现“自己等待自己”的死锁。
    bool reentrant = false;
    {
        std::unique_lock<std::mutex> lock(actor_mutex_);
        if (actor_running_ && actor_thread_id_ == std::this_thread::get_id()) {
            reentrant = true;
        } else {
            mailbox_.push_back(event);
            if (!actor_running_) {
                actor_running_ = true;
                if (zco::in_coroutine() &&
                    (actor_sched_id_ < 0 ||
                     zco::sched_id() == actor_sched_id_)) {
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
            zco::go([self]() { self->drain_mailbox(); });
        }
    }

    event->completion.wait();

    if (event->error != 0) {
        errno = event->error;
    }
    return event->result;
}

bool TcpConnection::try_begin_inline_actor() {
    if (!zco::in_coroutine()) {
        return false;
    }

    if (actor_sched_id_ >= 0 && zco::sched_id() != actor_sched_id_) {
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

void TcpConnection::finish_inline_actor() { drain_mailbox(); }

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
                // 邮箱耗尽后释放 actor_running_，下一个事件可重新拉起 worker。
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

void TcpConnection::process_event(const std::shared_ptr<Event> &event) {
    errno = 0;

    switch (event->type) {
    case EventType::kRead:
        event->result = read_internal(event->max_read_bytes, event->timeout_ms);
        break;
    case EventType::kSend:
        event->result = send_internal(event->payload.data(),
                                      event->payload.size(), event->timeout_ms);
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

ssize_t TcpConnection::read_internal(size_t max_read_bytes,
                                     uint32_t timeout_ms) {
    const State current = state();
    if ((current != State::kConnected && current != State::kDisconnecting) ||
        !socket_ || !socket_->is_valid()) {
        errno = EBADF;
        ZNET_LOG_WARN(
            "TcpConnection::read invalid state/socket: fd={}, state={}", fd(),
            state_to_string(current));
        return -1;
    }

    if (max_read_bytes == 0) {
        errno = EINVAL;
        ZNET_LOG_WARN("TcpConnection::read max_read_bytes must be > 0");
        return -1;
    }

    if (tls_channel_) {
        // TLS 已启用时改走 TLS 读路径，由 TLS 层处理解密与
        // WANT_READ/WANT_WRITE。
        return read_tls_internal(max_read_bytes, timeout_ms);
    }

    int saved_errno = 0;
    const ssize_t n = input_buffer_.read_from_socket(socket_, max_read_bytes,
                                                     timeout_ms, &saved_errno);
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

bool TcpConnection::enable_tls_server(
    const std::shared_ptr<TlsContext> &tls_context,
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
        ZNET_LOG_ERROR("TcpConnection::enable_tls_server create_server_channel "
                       "failed: fd={}, errno={}",
                       fd(), errno);
        return false;
    }

    if (!channel->handshake(handshake_timeout_ms,
                            [this](bool wait_for_write, uint32_t timeout_ms) {
                                return wait_tls_io(wait_for_write, timeout_ms);
                            })) {
        ZNET_LOG_WARN("TcpConnection::enable_tls_server handshake failed: "
                      "fd={}, errno={}",
                      fd(), errno);
        return false;
    }

    tls_channel_ = std::move(channel);
    ZNET_LOG_INFO("TcpConnection::enable_tls_server handshake success: fd={}",
                  fd());
    return true;
}

bool TcpConnection::wait_tls_io(bool wait_for_write, uint32_t timeout_ms) {
    // 把 TLS 层“等可读/可写”请求桥接到 zco IoEvent。
    zco::IoEvent io_event(fd(), wait_for_write ? zco::IoEventType::kWrite
                                               : zco::IoEventType::kRead);
    const uint32_t wait_timeout_ms =
        timeout_ms == 0 ? zco::kInfiniteTimeoutMs : timeout_ms;
    if (!io_event.wait(wait_timeout_ms)) {
        if (errno == 0 && zco::timeout()) {
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
    // TLS 读取落到临时缓冲，再 append 到输入缓冲，保持与非 TLS
    // 路径一致的消费模型。
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
        // TLS 路径按 output_buffer_ 内容循环写，直到写空或遇到阻塞/错误。
        while (output_buffer_.readable_bytes() > 0) {
            const char *data = output_buffer_.peek();
            const size_t bytes = output_buffer_.readable_bytes();
            const ssize_t n = write_tls_internal(data, bytes, timeout_ms);
            if (n <= 0) {
                if (n < 0) {
                    ZNET_LOG_WARN("TcpConnection::flush_output tls write "
                                  "failed: fd={}, errno={}",
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
            // 非阻塞写暂时不可写，保留剩余数据供后续 flush。
            break;
        }

        errno = saved_errno;
        ZNET_LOG_WARN(
            "TcpConnection::flush_output socket write failed: fd={}, errno={}",
            fd(), errno);
        return -1;
    }

    if (sent_total > 0 && output_buffer_.readable_bytes() == 0 &&
        write_complete_callback_) {
        write_complete_callback_(shared_from_this());
    }

    return sent_total;
}

ssize_t TcpConnection::write_tls_internal(const char *data, size_t length,
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

ssize_t TcpConnection::send_internal(const char *data, size_t length,
                                     uint32_t timeout_ms) {
    const State current = state();
    if (current != State::kConnected && current != State::kDisconnecting) {
        errno = EBADF;
        ZNET_LOG_WARN("TcpConnection::send invalid state: fd={}, state={}",
                      fd(), state_to_string(current));
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
    // 高水位回调只在“首次跨线”时触发，避免每次 send 都重复告警。
    if (high_water_mark_callback_ && old_pending < high_water_mark_ &&
        projected_pending >= high_water_mark_) {
        high_water_mark_callback_(shared_from_this(), projected_pending);
    }

    size_t buffered_offset = 0;
    if (!tls_channel_ && old_pending == 0) {
        // 非 TLS 且发送队列为空时，尝试一次直写减少内存拷贝。
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

    if (state() == State::kDisconnecting &&
        output_buffer_.readable_bytes() == 0) {
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

    // 半关闭语义：先尽力刷出发送缓冲，再做 TLS shutdown，最后关闭底层 socket。
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
        // inline 模式下调用者自己暂时承担 actor，结束后必须 drain 剩余邮箱。
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

ssize_t TcpConnection::send(const void *data, size_t length,
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
        const ssize_t result = send_internal(static_cast<const char *>(data),
                                             length, effective_timeout_ms);
        const int saved_errno = errno;
        finish_inline_actor();
        errno = saved_errno;
        return result;
    }

    std::shared_ptr<Event> event = std::make_shared<Event>(EventType::kSend);
    event->timeout_ms = effective_timeout_ms;
    event->payload.assign(static_cast<const char *>(data), length);
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

    std::shared_ptr<Event> event =
        std::make_shared<Event>(EventType::kShutdown);
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

} // namespace znet
