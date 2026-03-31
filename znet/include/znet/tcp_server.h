#ifndef ZNET_TCP_SERVER_H_
#define ZNET_TCP_SERVER_H_

#include "acceptor.h"
#include "callbacks.h"
#include "server.h"
#include "session.h"
#include "tcp_connection.h"

#include <memory>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <utility>

namespace zcoroutine {
class Scheduler;
}

namespace znet {

/**
 * @brief 基于协程调度器的 TCP 服务器实现。
 *
 * 核心职责：
 * 1. 管理 Acceptor 监听与新连接分发。
 * 2. 为每条连接创建 TcpConnection + Stream 读写通道。
 * 3. 维护连接表，并在连接生命周期中触发各类业务回调。
 *
 * 生命周期概览：
 * - do_start(): 初始化协程调度器、启动 acceptor。
 * - handle_connection(): 为新连接建立读循环并驱动 on_message 回调。
 * - do_stop(): 停止 acceptor 并关闭所有连接。
 */
class TcpServer : public Server, public std::enable_shared_from_this<TcpServer> {
 public:
  using ptr = std::shared_ptr<TcpServer>;
  using ConnectionMap = std::unordered_map<int, TcpConnection::ptr>;

  /**
   * @brief 读写流工厂。
   *
   * 返回值语义：
   * - first: 读流（应用层消费入站数据）。
   * - second: 写流（应用层写入待发送数据）。
   */
  using StreamFactory = std::function<std::pair<Stream::ptr, Stream::ptr>()>;

  /**
   * @param listen_address 监听地址。
   * @param backlog 内核监听队列长度。
   */
  explicit TcpServer(Address::ptr listen_address, int backlog = SOMAXCONN);
  ~TcpServer() override = default;

  /**
   * @brief 设置协程调度器线程数。
   *
   * 说明：该值会传入 zcoroutine::init(thread_count)。
   */
  void set_thread_count(int thread_count) { thread_count_ = thread_count; }

  /**
   * @brief 设置自定义流工厂。
   *
   * 未设置时默认使用 BufferStream 作为读流和写流。
   */
  void set_stream_factory(StreamFactory factory) {
    stream_factory_ = std::move(factory);
  }

  /**
   * @brief 设置消息回调。
   */
  void set_on_message(MessageCallback callback) {
    on_message_callback_ = std::move(callback);
  }

  /**
   * @brief 设置关闭回调。
   */
  void set_on_close(CloseCallback callback) {
    on_close_callback_ = std::move(callback);
  }

  /**
   * @brief 设置连接建立回调。
   */
  void set_on_connection(ConnectionCallback callback) {
    on_connection_callback_ = std::move(callback);
  }

  /**
   * @brief 设置写完成回调。
   */
  void set_on_write_complete(WriteCompleteCallback callback) {
    on_write_complete_callback_ = std::move(callback);
  }

  /**
   * @brief 设置高水位回调。
   * @param callback 跨越高水位阈值时触发。
   * @param high_water_mark 高水位阈值（字节）。
   */
  void set_on_high_water_mark(HighWaterMarkCallback callback,
                              size_t high_water_mark) {
    on_high_water_mark_callback_ = std::move(callback);
    high_water_mark_ = high_water_mark;
  }

  /**
   * @brief 获取底层接入器。
   */
  std::shared_ptr<Acceptor> acceptor() const { return acceptor_; }

 protected:
  bool do_start() override;
  void do_stop() override;

 private:
  /**
   * @brief 新连接处理入口。
   */
  void handle_connection(Socket::ptr client);

  /**
   * @brief 从连接表移除连接。
   */
  void remove_connection(int fd);

  /**
   * @brief 将连接登记到连接表。
   */
  void register_connection(const TcpConnection::ptr& connection);

  /**
   * @brief 创建读写流对。
   */
  std::pair<Stream::ptr, Stream::ptr> create_stream_pair() const;

 private:
  std::shared_ptr<Acceptor> acceptor_;
  MessageCallback on_message_callback_;
  ConnectionCallback on_connection_callback_;
  CloseCallback on_close_callback_;
  WriteCompleteCallback on_write_complete_callback_;

  HighWaterMarkCallback on_high_water_mark_callback_;
  size_t high_water_mark_;

  int thread_count_; // 初始化底层线程数量，默认为 CPU 核数
  StreamFactory stream_factory_;

  zcoroutine::Scheduler* connection_registry_sched_;

  ConnectionMap connections_;
};

}  // namespace znet

#endif  // ZNET_TCP_SERVER_H_
