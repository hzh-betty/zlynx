#ifndef ZNET_TCP_CONNECTION_H_
#define ZNET_TCP_CONNECTION_H_

#include "address.h"
#include "buff.h"
#include "noncopyable.h"
#include "socket.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "spinlock.h"

// 前置声明
namespace zcoroutine {
class IoScheduler;
}

namespace znet {

// 前向声明
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

/**
 * @brief TCP 连接回调函数类型
 */
using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
using MessageCallback = std::function<void(const TcpConnectionPtr &, Buffer *)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr &)>;
using CloseCallback = std::function<void(const TcpConnectionPtr &)>;

/**
 * @brief TCP 连接对象
 * 管理单个 TCP 连接的生命周期、缓冲区、IO 事件
 */
class TcpConnection : public NonCopyable,
                      public std::enable_shared_from_this<TcpConnection> {
public:
  /**
   * @brief 连接状态
   */
  enum class State {
    Connecting = 0,    // 正在连接
    Connected = 1,     // 已连接
    Disconnecting = 2, // 正在断开
    Disconnected = 3,  // 已断开
  };

  /**
   * @brief 构造函数
   * @param name 连接名称
   * @param socket Socket 对象（所有权转移）
   * @param local_addr 本地地址
   * @param peer_addr 对端地址
   * @param io_scheduler IO调度器
   */
  TcpConnection(std::string name, Socket::ptr socket,
                const Address::ptr &local_addr, const Address::ptr &peer_addr,
                zcoroutine::IoScheduler *io_scheduler = nullptr);

  ~TcpConnection();

  /**
   * @brief 获取连接名称
   */
  const std::string &name() const { return name_; }

  /**
   * @brief 获取本地地址
   */
  Address::ptr local_address() const { return local_addr_; }

  /**
   * @brief 获取对端地址
   */
  Address::ptr peer_address() const { return peer_addr_; }

  /**
   * @brief 获取连接状态
   */
  State state() const { return state_.load(std::memory_order_acquire); }

  /**
   * @brief 是否已连接
   */
  bool connected() const {
    return state_.load(std::memory_order_acquire) == State::Connected;
  }

  /**
   * @brief 是否已断开
   */
  bool disconnected() const {
    return state_.load(std::memory_order_acquire) == State::Disconnected;
  }

  void set_connection_callback(ConnectionCallback cb) {
    connection_callback_ = std::move(cb);
  }

  void set_message_callback(MessageCallback cb) {
    message_callback_ = std::move(cb);
  }

  void set_write_complete_callback(WriteCompleteCallback cb) {
    write_complete_callback_ = std::move(cb);
  }

  void set_close_callback(CloseCallback cb) { close_callback_ = std::move(cb); }

  /**
   * @brief 连接建立时调用（由 TcpServer 调用）
   */
  void connect_established();

  /**
   * @brief 发送数据
   * @param data 数据指针
   * @param len 数据长度
   */
  void send(const void *data, size_t len);

  /**
   * @brief 发送字符串
   */
  void send(const std::string &message);

  /**
   * @brief 发送 Buffer
   */
  void send(Buffer *buf);

  /**
   * @brief 关闭连接（只关闭写端）
   */
  void shutdown();

  /**
   * @brief 强制关闭连接
   */
  void force_close();

  /**
   * @brief 设置 TCP_NODELAY
   */
  void set_tcp_no_delay(bool on);

  /**
   * @brief 设置 SO_KEEPALIVE
   */
  void set_keep_alive(bool on);

  /**
   * @brief 可读事件处理
   */
  void handle_read();

  /**
   * @brief 可写事件处理
   */
  void handle_write();

  /**
   * @brief 关闭事件处理
   */
  void handle_close();

  /**
   * @brief 错误事件处理
   */
  void handle_error();

  /**
   * @brief 获取输入缓冲区
   */
  Buffer *input_buffer() { return &input_buffer_; }

  /**
   * @brief 获取输出缓冲区
   */
  Buffer *output_buffer() { return &output_buffer_; }

  /**
   * @brief 获取 Socket 对象
   */
  Socket::ptr socket() { return socket_; }

  /**
   * @brief 获取 IoScheduler
   */
  zcoroutine::IoScheduler *io_scheduler() { return io_scheduler_; }

  /**
   * @brief 将状态转换为字符串
   */
  static const char *state_to_string(State s);

private:
  /**
   * @brief 设置连接状态（线程安全）
   */
  void set_state(State s) { state_.store(s, std::memory_order_release); }

  /**
   * @brief 在 IO 线程中发送数据
   */
  void send_in_loop(const void *data, size_t len);

  /**
   * @brief 在 IO 线程中关闭连接
   */
  void shutdown_in_loop();

  /**
   * @brief 在 IO 线程中强制关闭
   */
  void force_close_in_loop();

private:
  std::string name_;         // 连接名称
  std::atomic<State> state_; // 连接状态（原子变量保证线程安全）
  Socket::ptr socket_;       // Socket 对象
  Address::ptr local_addr_;  // 本地地址
  Address::ptr peer_addr_;   // 对端地址

  zcoroutine::IoScheduler *io_scheduler_; // IO调度器

  Buffer input_buffer_;                             // 输入缓冲区
  Buffer output_buffer_;                            // 输出缓冲区
  mutable zcoroutine::Spinlock output_buffer_lock_; // 输出缓冲区锁

  // 回调函数
  ConnectionCallback connection_callback_;
  MessageCallback message_callback_;
  WriteCompleteCallback write_complete_callback_;
  CloseCallback close_callback_;
};

} // namespace znet

#endif // ZNET_TCP_CONNECTION_H_
