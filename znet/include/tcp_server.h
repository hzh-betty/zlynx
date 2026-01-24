#ifndef ZNET_TCP_SERVER_H
#define ZNET_TCP_SERVER_H

#include "address.h"
#include "socket.h"
#include "tcp_connection.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "io_scheduler.h"
#include "rw_mutex.h"

namespace znet {

/**
 * @brief TCP 服务器封装
 */
class TcpServer : public std::enable_shared_from_this<TcpServer> {
public:
  using ptr = std::shared_ptr<TcpServer>;

  /**
   * @brief 构造函数
   * @param io_worker socket 客户端工作的协程调度器
   * @param accept_worker 服务器 socket 执行接收 socket
   * 连接的协程调度器（默认创建单线程调度器）
   */
  TcpServer(zcoroutine::IoScheduler::ptr io_worker,
            zcoroutine::IoScheduler::ptr accept_worker = nullptr);

  /**
   * @brief 析构函数
   */
  virtual ~TcpServer();

  /**
   * @brief 绑定地址
   * @return 返回是否绑定成功
   */
  bool bind(Address::ptr addr);

  /**
   * @brief 绑定地址数组
   * @param addrs 需要绑定的地址数组
   * @param fails 绑定失败的地址
   * @return 是否绑定成功
   */
  bool bind(const std::vector<Address::ptr> &addrs,
            std::vector<Address::ptr> &fails);

  /**
   * @brief 启动服务
   * @pre 需要 bind 成功后执行
   */
  bool start();

  /**
   * @brief 停止服务
   */
  void stop();

  /**
   * @brief 返回读取超时时间(毫秒)
   */
  uint64_t recv_timeout() const { return recv_timeout_; }

  /**
   * @brief 返回服务器名称
   */
  std::string name() const { return name_; }

  /**
   * @brief 设置读取超时时间(毫秒)
   */
  void set_recv_timeout(uint64_t v) { recv_timeout_ = v; }

  /**
   * @brief 设置服务器名称
   */
  virtual void set_name(const std::string &v) { name_ = v; }

  /**
   * @brief 是否停止
   */
  bool is_stop() const { return is_stop_.load(std::memory_order_acquire); }

  /**
   * @brief 以字符串形式 dump server 信息
   */
  std::string to_string(const std::string &prefix = "");

protected:
  /**
   * @brief 处理新连接的 TcpConnection
   * @note connect_established() 已自动调用，子类无需手动调用
   */
  virtual void handle_client(TcpConnectionPtr conn);

  /**
   * @brief 开始接受连接
   */
  void start_accept(Socket::ptr sock);

protected:
  std::vector<Socket::ptr> socks_;         // 监听 Socket 数组
  mutable zcoroutine::RWMutex socks_mutex_; // socks_ 读写锁保护
  zcoroutine::IoScheduler::ptr io_worker_; // 新连接的 Socket 工作的调度器
  zcoroutine::IoScheduler::ptr
      accept_worker_; // 服务器 Socket 接收连接的调度器（默认单线程）
  uint64_t recv_timeout_; // 接收超时时间(毫秒)
  std::string name_;      // 服务器名称
  std::string type_;      // 服务器类型
  std::atomic<bool> is_stop_;  // 服务是否停止
};

} // namespace znet

#endif // ZNET_TCP_SERVER_H
