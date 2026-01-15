#ifndef ZNET_SOCKET_H_
#define ZNET_SOCKET_H_

#include "address.h"
#include "noncopyable.h"
#include <memory>
#include <sys/socket.h>

namespace znet {

/**
 * @brief Socket 封装类
 * 提供 Socket 的创建、连接、读写、配置等功能
 * 支持 TCP/UDP 协议
 * 集成 IoScheduler，支持异步IO和协程调度
 */
class Socket : public std::enable_shared_from_this<Socket>, public NonCopyable {
public:
  using ptr = std::shared_ptr<Socket>;

  /**
   * @brief 构造函数，创建 Socket
   * @param family 地址族 (AF_INET/AF_INET6)
   * @param type 类型 (SOCK_STREAM/SOCK_DGRAM)
   * @param protocol 协议 (通常为 0)
   */
  Socket(int family, int type, int protocol = 0);

  /**
   * @brief 从已有的 fd 构造
   */
  explicit Socket(int sockfd);

  /**
   * @brief 通过地址类型创建 TCP Socket
   * @param address 地址
   */
  static Socket::ptr create_tcp(Address::ptr address);

  ~Socket();

  /**
   * @brief 获取 socket fd
   */
  int fd() const { return sockfd_; }

  /**
   * @brief 获取地址族
   */
  int family() const { return family_; }

  /**
   * @brief 获取 socket 类型
   */
  int type() const { return type_; }

  /**
   * @brief socket 是否有效
   */
  bool is_valid() const { return sockfd_ != -1; }

  /**
   * @brief 创建 TCP Socket (IPv4)
   */
  static Socket::ptr create_tcp();

  /**
   * @brief 创建 TCP Socket (IPv6)
   */
  static Socket::ptr create_tcp_v6();

  /**
   * @brief 创建 UDP Socket (IPv4)
   */
  static Socket::ptr create_udp();

  /**
   * @brief 创建 UDP Socket (IPv6)
   */
  static Socket::ptr create_udp_v6();

  /**
   * @brief 绑定地址
   * @return 成功返回 true
   */
  bool bind(const Address::ptr addr);

  /**
   * @brief 监听
   * @param backlog 监听队列长度
   * @return 成功返回 true
   */
  bool listen(int backlog = SOMAXCONN);

  /**
   * @brief 接受连接
   * @return 新连接的 Socket，失败返回 nullptr
   */
  Socket::ptr accept();

  /**
   * @brief 连接到远端地址
   * @param addr 远端地址
   * @param timeout_ms 超时时间（毫秒），0 表示阻塞
   * @return 成功返回 true
   */
  bool connect(const Address::ptr addr, uint64_t timeout_ms = 0);

  /**
   * @brief 重新连接
   * @param timeout_ms 超时时间（毫秒）
   * @return 成功返回 true
   */
  bool reconnect(uint64_t timeout_ms = 0);

  /**
   * @brief 关闭 socket
   */
  bool close();

  /**
   * @brief 关闭写端（半关闭）
   */
  bool shutdown_write();


  /**
   * @brief 发送数据
   * @param buffer 数据缓冲区
   * @param length 数据长度
   * @param flags 发送标志
   * @return 发送的字节数，-1 表示错误
   */
  ssize_t send(const void *buffer, size_t length, int flags = 0);

  /**
   * @brief 接收数据
   * @param buffer 接收缓冲区
   * @param length 缓冲区长度
   * @param flags 接收标志
   * @return 接收的字节数，-1 表示错误，0 表示连接关闭
   */
  ssize_t recv(void *buffer, size_t length, int flags = 0);

  /**
   * @brief 发送数据到指定地址 (UDP)
   */
  ssize_t send_to(const void *buffer, size_t length, const Address::ptr to,
                  int flags = 0);

  /**
   * @brief 从指定地址接收数据 (UDP)
   */
  ssize_t recv_from(void *buffer, size_t length, Address::ptr from,
                    int flags = 0);

  /**
   * @brief 设置 socket 选项
   * @param level 选项级别 (SOL_SOCKET, IPPROTO_TCP 等)
   * @param option 选项名
   * @param value 选项值
   */
  template <typename T> bool set_option(int level, int option, const T &value) {
    return setsockopt(sockfd_, level, option, &value, sizeof(T)) == 0;
  }

  /**
   * @brief 获取 socket 选项
   */
  template <typename T> bool get_option(int level, int option, T *value) {
    socklen_t len = sizeof(T);
    return getsockopt(sockfd_, level, option, value, &len) == 0;
  }

  /**
   * @brief 设置发送超时
   */
  bool set_send_timeout(uint64_t timeout_ms);

  /**
   * @brief 设置接收超时
   */
  bool set_recv_timeout(uint64_t timeout_ms);

  /**
   * @brief 设置 TCP_NODELAY (禁用 Nagle 算法)
   */
  bool set_tcp_nodelay(bool on);

  /**
   * @brief 设置 SO_REUSEADDR
   */
  bool set_reuse_addr(bool on);

  /**
   * @brief 设置 SO_REUSEPORT
   */
  bool set_reuse_port(bool on);

  /**
   * @brief 设置 SO_KEEPALIVE
   */
  bool set_keep_alive(bool on);

  /**
   * @brief 设置非阻塞模式
   */
  bool set_non_blocking(bool on);


  /**
   * @brief 获取本地地址
   */
  Address::ptr get_local_address();

  /**
   * @brief 获取远端地址
   */
  Address::ptr get_remote_address();

  /**
   * @brief 获取 socket 错误
   */
  int get_error();

  /**
   * @brief 取消读事件
   * @return 成功返回 true
   */
  bool cancel_read();

  /**
   * @brief 取消写事件
   * @return 成功返回 true
   */
  bool cancel_write();

  /**
   * @brief 取消accept事件
   * @return 成功返回 true
   */
  bool cancel_accept();

  /**
   * @brief 取消所有事件
   * @return 成功返回 true
   */
  bool cancel_all();

private:
  /**
   * @brief 初始化 socket
   */
  void init_sock();

  /**
   * @brief 创建新的 socket
   */
  bool new_sock();

private:
  int sockfd_;   // socket 文件描述符
  int family_;   // 地址族
  int type_;     // socket 类型
  int protocol_; // 协议

  bool is_connected_; // 是否已连接

  Address::ptr local_address_;  // 本地地址
  Address::ptr remote_address_; // 远端地址
};

} // namespace znet

#endif // ZNET_SOCKET_H_
