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
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace zcoroutine {
class Scheduler;
}

namespace znet {

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

  ssize_t read(size_t max_read_bytes = 4096, uint32_t timeout_ms = 0);
  ssize_t flush_output(uint32_t timeout_ms = 0);
  ssize_t send(const void* data, size_t length, uint32_t timeout_ms = 0);

  void shutdown();
  void close();

 private:
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

  ssize_t dispatch_event_and_wait(const Event::ptr& event);
  void drain_mailbox();
  void process_event(const Event::ptr& event);

  ssize_t read_internal(size_t max_read_bytes, uint32_t timeout_ms);
  ssize_t flush_output_internal(uint32_t timeout_ms);
  ssize_t send_internal(const std::string& payload, uint32_t timeout_ms);
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
