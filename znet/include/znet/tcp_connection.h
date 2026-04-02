#ifndef ZNET_TCP_CONNECTION_H_
#define ZNET_TCP_CONNECTION_H_

#include "buffer.h"
#include "noncopyable.h"
#include "socket.h"
#include "zcoroutine/wait_group.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace zcoroutine {
class Scheduler;
}

namespace znet {

class TcpServer;
class TlsContext;
class TlsChannel;

/**
 * @brief TCP 连接对象，封装 socket + 输入输出缓冲 + 连接状态机。
 *
 * 并发模型：每个连接内部维护一个 Actor 邮箱，所有读写/关闭事件都通过邮箱
 * 串行执行，避免依赖固定调度器亲和性。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection>,
                      public NonCopyable {
 public:
  using ptr = std::shared_ptr<TcpConnection>;
  static constexpr uint32_t kUseConnectionWriteTimeout =
      std::numeric_limits<uint32_t>::max();

  using WriteCompleteCallback = std::function<void(TcpConnection::ptr)>;
  using HighWaterMarkCallback = std::function<void(TcpConnection::ptr, size_t)>;

  enum class State : uint8_t {
    kDisconnected = 0,
    kConnecting = 1,
    kConnected = 2,
    kDisconnecting = 3,
  };

  explicit TcpConnection(Socket::ptr socket,
                         zcoroutine::Scheduler* actor_scheduler = nullptr);

  ~TcpConnection();

  int fd() const;

  bool connected() const {
    return static_cast<State>(state_.load(std::memory_order_acquire)) ==
           State::kConnected;
  }

  State state() const {
    return static_cast<State>(state_.load(std::memory_order_acquire));
  }

  Socket::ptr socket() const { return socket_; }

  Buffer& input_buffer() { return input_buffer_; }
  const Buffer& input_buffer() const { return input_buffer_; }

  Buffer& output_buffer() { return output_buffer_; }
  const Buffer& output_buffer() const { return output_buffer_; }

  void set_write_complete_callback(WriteCompleteCallback callback) {
    write_complete_callback_ = std::move(callback);
  }

  void set_high_water_mark_callback(HighWaterMarkCallback callback,
                                    size_t high_water_mark) {
    high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = high_water_mark;
  }

  void set_context(void* ctx) { context_ = ctx; }
  void* context() const { return context_; }

  void set_write_timeout(uint32_t timeout_ms) {
    write_timeout_ms_.store(timeout_ms, std::memory_order_release);
  }

  uint32_t write_timeout() const {
    return write_timeout_ms_.load(std::memory_order_acquire);
  }

  ssize_t read(size_t max_read_bytes = 4096, uint32_t timeout_ms = 0);
  ssize_t flush_output(
      uint32_t timeout_ms = kUseConnectionWriteTimeout);
  ssize_t send(const void* data,
               size_t length,
               uint32_t timeout_ms = kUseConnectionWriteTimeout);

  void shutdown();
  void close();

  bool is_tls_enabled() const { return tls_channel_ != nullptr; }

 private:
  friend class TcpServer;

  bool enable_tls_server(const std::shared_ptr<TlsContext>& tls_context,
                         uint32_t handshake_timeout_ms);

  bool wait_tls_io(bool wait_for_write, uint32_t timeout_ms);

  enum class EventType : uint8_t {
    kRead = 0,
    kSend = 1,
    kFlush = 2,
    kShutdown = 3,
    kClose = 4,
  };

  struct Event {
    using ptr = std::shared_ptr<Event>;

    explicit Event(EventType t)
        : type(t),
          max_read_bytes(0),
          timeout_ms(0),
          payload(),
          result(0),
          error(0),
          completion(1) {}

    EventType type;
    size_t max_read_bytes;
    uint32_t timeout_ms;
    std::string payload;
    ssize_t result;
    int error;
    zcoroutine::WaitGroup completion;
  };

 private:
  void set_state(State state);
  size_t pending_write_bytes() const;
  uint32_t resolve_write_timeout(uint32_t timeout_ms) const;

  ssize_t dispatch_event_and_wait(const Event::ptr& event);
  void drain_mailbox();
  void process_event(const Event::ptr& event);

  ssize_t read_internal(size_t max_read_bytes, uint32_t timeout_ms);
  ssize_t read_tls_internal(size_t max_read_bytes, uint32_t timeout_ms);
  ssize_t flush_output_internal(uint32_t timeout_ms);
  ssize_t write_tls_internal(const char* data,
                             size_t length,
                             uint32_t timeout_ms);
  ssize_t send_internal(const std::string& payload, uint32_t timeout_ms);
  void shutdown_tls_internal();
  void close_tls_internal();
  void shutdown_internal();
  void close_internal();

 private:
  Socket::ptr socket_;
  Buffer input_buffer_;
  Buffer output_buffer_;

  std::atomic<uint8_t> state_;

  WriteCompleteCallback write_complete_callback_;
  HighWaterMarkCallback high_water_mark_callback_;
  size_t high_water_mark_;
  std::atomic<uint32_t> write_timeout_ms_;

  std::unique_ptr<TlsChannel> tls_channel_;

  void* context_;

  mutable std::mutex actor_mutex_;
  std::deque<Event::ptr> mailbox_;
  bool actor_running_; // actor 是否正在运行
  std::thread::id actor_thread_id_;
  zcoroutine::Scheduler* actor_scheduler_;
  int actor_sched_id_;
};

}  // namespace znet

#endif  // ZNET_TCP_CONNECTION_H_
