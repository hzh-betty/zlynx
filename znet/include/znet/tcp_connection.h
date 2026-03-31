#ifndef ZNET_TCP_CONNECTION_H_
#define ZNET_TCP_CONNECTION_H_

#include "noncopyable.h"
#include "session.h"
#include "socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/types.h>

namespace zcoroutine {
class Scheduler;
}

namespace znet {

/**
 * @brief TCP 连接对象，封装 socket + 输入输出流 + 连接状态机。
 *
 * 该类主要做三件事：
 * 1. 驱动 read_stream 执行 read_to_buffer（read_once）。
 * 2. 驱动 write_stream 执行 flush_buffer（flush_output）。
 * 3. 维护连接状态与所属调度器，保证协程上下文中的线程亲和性。
 *
 * 并发模型：类似muduo的one loop per thread 设计，但这里的“loop”是 zcoroutine::Scheduler。
 * - 连接可绑定到一个 owner scheduler（owner_sched_id_）。
 * - 某些操作（如 flush_output）要求在 owner loop 内执行。
 * - 非 owner loop 调用 send() 时，会自动投递到 owner scheduler 异步执行。
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection>,
                      public NonCopyable {
 public:
  using ptr = std::shared_ptr<TcpConnection>;
  using WriteCompleteCallback = std::function<void(TcpConnection::ptr)>;
  using HighWaterMarkCallback = std::function<void(TcpConnection::ptr, size_t)>;

  /**
   * @brief 连接状态机。
   */
  enum class State : uint8_t {
    kDisconnected = 0,
    kConnecting = 1,
    kConnected = 2,
    kDisconnecting = 3,
  };

  /**
   * @param socket 底层 socket，允许为空（空时连接会直接处于断开态）。
   * @param read_stream 读流，若为空会自动创建 BufferStream。
   * @param write_stream 写流，若为空会自动创建 BufferStream。
   */
  explicit TcpConnection(Socket::ptr socket,
                         Stream::ptr read_stream = nullptr,
                         Stream::ptr write_stream = nullptr);

  /**
   * @brief 获取底层 fd。
   * @return fd；若 socket 不存在返回 -1。
   */
  int fd() const;

  /**
   * @brief 当前连接是否处于 kConnected。
   */
  bool connected() const {
    return static_cast<State>(state_.load(std::memory_order_acquire)) ==
           State::kConnected;
  }

  /**
   * @brief 获取当前状态。
   */
  State state() const {
    return static_cast<State>(state_.load(std::memory_order_acquire));
  }

  /**
   * @brief 返回 owner scheduler 的 id，未绑定时为 -1。
   */
  int owner_sched_id() const { return owner_sched_id_; }

  /**
   * @brief 绑定到当前协程调度器。
   */
  void bind_to_current_loop();

  /**
   * @brief 显式绑定到指定调度器。
   * @param scheduler 目标调度器；为空时退化为 bind_to_current_loop()。
   */
  void bind_to_loop(zcoroutine::Scheduler* scheduler);

  /**
   * @brief 获取底层 socket。
   */
  Socket::ptr socket() const { return socket_; }

  /**
   * @brief 获取读流。
   */
  Stream::ptr read_stream() const { return read_stream_; }

  /**
   * @brief 获取写流。
   */
  Stream::ptr write_stream() const { return write_stream_; }

  /**
   * @brief 设置读流。
   * @param read_stream 新读流；为空时自动创建 BufferStream。
   */
  void set_read_stream(Stream::ptr read_stream);

  /**
   * @brief 设置写流。
   * @param write_stream 新写流；为空时自动创建 BufferStream。
   */
  void set_write_stream(Stream::ptr write_stream);

  /**
   * @brief 同时设置读写流。
   */
  void set_streams(Stream::ptr read_stream, Stream::ptr write_stream);

  /**
   * @brief 设置写完成回调。
   */
  void set_write_complete_callback(WriteCompleteCallback callback) {
    write_complete_callback_ = std::move(callback);
  }

  /**
   * @brief 设置高水位回调与阈值。
   * @param callback 高水位回调。
   * @param high_water_mark 高水位阈值（字节）。
   */
  void set_high_water_mark_callback(HighWaterMarkCallback callback,
                                    size_t high_water_mark) {
    high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = high_water_mark;
  }

  /**
   * @brief 设置上下文指针。
   * @param ctx 新的上下文指针。
   */
  void set_context(void* ctx) { context_ = ctx; }
  
  /**
   * @brief 获取上下文指针。
   * @return 上下文指针。
   */
  void* context() const { return context_; }

  /**
  * @brief 驱动 read_stream 执行一次读入。
   * @param max_read_bytes 单次最多读取字节数。
   * @param timeout_ms 超时（毫秒），0 表示沿用 socket 当前设置。
   * @return >0 读取字节数；0 对端关闭；<0 失败（errno 表示原因）。
   */
  ssize_t read(size_t max_read_bytes = 4096, uint32_t timeout_ms = 0);

  /**
  * @brief 驱动 write_stream 尽可能刷出待发送数据。
   * @param timeout_ms 超时（毫秒），0 表示沿用 socket 当前设置。
   * @return >=0 本次写出总字节数；<0 失败。
   *
   * 注意：若连接绑定了 owner loop，必须在 owner loop 中调用。
   */
  ssize_t flush_output(uint32_t timeout_ms = 0);

  /**
   * @brief 发送数据。
   * @param data 数据首地址。
   * @param length 数据长度。
   * @param timeout_ms 超时（毫秒）。
   * @return >=0 已接收的发送长度；<0 失败。
   *
   * 当调用方不在 owner loop 内时，可能退化为“异步投递发送任务”。
   */
  ssize_t send(const void* data, size_t length, uint32_t timeout_ms = 0);

  /**
   * @brief 优雅关闭：先进入 kDisconnecting，再尝试刷出残留数据。
   */
  void shutdown();

  /**
   * @brief 立即关闭连接并释放底层 socket。
   */
  void close();

 private:
  void bind_stream(const Stream::ptr& stream);
  void set_state(State state);
  bool in_owner_loop() const;
  ssize_t send_in_loop(const void* data, size_t length, uint32_t timeout_ms);
  size_t pending_write_bytes() const;

 private:
  Socket::ptr socket_;
  Stream::ptr read_stream_;
  Stream::ptr write_stream_;

  std::atomic<uint8_t> state_;

  zcoroutine::Scheduler* owner_scheduler_;
  int owner_sched_id_; // 所属协程调度器 ID
  
  WriteCompleteCallback write_complete_callback_;
  HighWaterMarkCallback high_water_mark_callback_;
  size_t high_water_mark_;

  void* context_; // 上下文指针，供外部使用（如协议解析器等）
};

}  // namespace znet

#endif  // ZNET_TCP_CONNECTION_H_
