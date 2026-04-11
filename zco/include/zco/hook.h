#ifndef ZCO_HOOK_H_
#define ZCO_HOOK_H_

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cstddef>
#include <cstdint>

#include "zco/sched.h"

/**
 * @brief 协程友好的系统调用包装。
 * @details 因为协程相关函数必须是非阻塞的，所以提供与系统调用语义一致的接口，
 *  同时集成协程调度逻辑和超时控制。对于不涉及 I/O 的系统调用，
 *  提供协程友好的包装以便在协程中使用。
 */

namespace zco {

/**
 * @brief 协程友好的 sleep 包装。
 * @param milliseconds 休眠时长。
 * @return 无返回值。
 */
void co_sleep_for(uint32_t milliseconds);

/**
 * @brief 获取当前线程错误码。
 * @return 当前 errno。
 */
int co_error();

/**
 * @brief 设置当前线程错误码。
 * @param error_code 目标错误码。
 */
void co_error(int error_code);

/**
 * @brief 显式同步 dup 后 fd 元数据。
 * @param from_fd 源 fd。
 * @param to_fd 新 fd。
 * @return 无返回值。
 */
void sync_fd_metadata_on_dup(int from_fd, int to_fd);

/**
 * @brief 显式清理 close 前 fd 元数据。
 * @param fd 文件描述符。
 * @return 无返回值。
 */
void sync_fd_metadata_on_close(int fd);

/**
 * @brief 创建协程网络编程适用的 socket（默认 non-blocking）。
 * @param domain 地址族。
 * @param type socket 类型。
 * @param protocol 协议号。
 * @return 成功返回 fd，失败返回 -1。
 */
int co_socket(int domain, int type, int protocol);

/**
 * @brief 创建 TCP socket（默认 non-blocking）。
 * @param domain 地址族。
 * @return 成功返回 fd，失败返回 -1。
 */
int co_tcp_socket(int domain = AF_INET);

/**
 * @brief 创建 UDP socket（默认 non-blocking）。
 * @param domain 地址族。
 * @return 成功返回 fd，失败返回 -1。
 */
int co_udp_socket(int domain = AF_INET);

/**
 * @brief 协程友好的 dup 包装（含元数据同步）。
 * @param oldfd 源 fd。
 * @return 新 fd，失败返回 -1。
 */
int co_dup(int oldfd);

/**
 * @brief 协程友好的 dup2 包装（含元数据同步）。
 * @param oldfd 源 fd。
 * @param newfd 目标 fd。
 * @return 返回 newfd，失败返回 -1。
 */
int co_dup2(int oldfd, int newfd);

/**
 * @brief 协程友好的 dup3 包装（含元数据同步）。
 * @param oldfd 源 fd。
 * @param newfd 目标 fd。
 * @param flags dup3 标志。
 * @return 返回 newfd，失败返回 -1。
 */
int co_dup3(int oldfd, int newfd, int flags);

/**
 * @brief 协程友好的 close 包装（含元数据清理）。
 * @param fd 文件描述符。
 * @param delay_ms 延迟关闭毫秒，仅协程上下文生效。
 * @return close 返回值。
 */
int co_close(int fd, uint32_t delay_ms = 0);

/**
 * @brief 重置 TCP 连接（SO_LINGER on/off=1/0），随后执行 close。
 * @param fd 文件描述符。
 * @param delay_ms 延迟关闭毫秒，仅协程上下文生效。
 * @return 成功返回 0，失败返回 -1。
 */
int co_reset_tcp_socket(int fd, uint32_t delay_ms = 0);

/**
 * @brief 协程友好的 shutdown 包装。
 * @param fd 文件描述符。
 * @param how 'r' 关闭读，'w' 关闭写，'b' 关闭读写。
 * @return shutdown 返回值。
 */
int co_shutdown(int fd, char how = 'b');

/**
 * @brief 协程友好的 bind 包装。
 * @param fd 文件描述符。
 * @param address 地址结构。
 * @param address_len 地址长度。
 * @return bind 返回值。
 */
int co_bind(int fd, const struct sockaddr *address, socklen_t address_len);

/**
 * @brief 协程友好的 listen 包装。
 * @param fd 文件描述符。
 * @param backlog 监听队列长度。
 * @return listen 返回值。
 */
int co_listen(int fd, int backlog = 1024);

/**
 * @brief 协程友好的 getsockopt 包装。
 */
int co_getsockopt(int fd, int level, int option, void *option_value,
                  socklen_t *option_len);

/**
 * @brief 协程友好的 setsockopt 包装。
 */
int co_setsockopt(int fd, int level, int option, const void *option_value,
                  socklen_t option_len);

/**
 * @brief 设置 socket 为 non-blocking。
 * @param fd 文件描述符。
 */
void co_set_nonblock(int fd);

/**
 * @brief 设置 socket 的 FD_CLOEXEC。
 * @param fd 文件描述符。
 */
void co_set_cloexec(int fd);

/**
 * @brief 设置 SO_REUSEADDR。
 * @param fd 文件描述符。
 */
void co_set_reuseaddr(int fd);

/**
 * @brief 设置 SO_SNDBUF。
 * @param fd 文件描述符。
 * @param bytes 缓冲区大小。
 */
void co_set_send_buffer_size(int fd, int bytes);

/**
 * @brief 设置 SO_RCVBUF。
 * @param fd 文件描述符。
 * @param bytes 缓冲区大小。
 */
void co_set_recv_buffer_size(int fd, int bytes);

/**
 * @brief 设置 SO_KEEPALIVE。
 * @param fd 文件描述符。
 */
void co_set_tcp_keepalive(int fd);

/**
 * @brief 设置 TCP_NODELAY。
 * @param fd 文件描述符。
 */
void co_set_tcp_nodelay(int fd);

/**
 * @brief 协程友好的 read 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 读取字节数。
 * @param timeout_ms 超时毫秒。
 * @return 读取结果，与系统调用 read 语义一致。
 */
ssize_t co_read(int fd, void *buffer, size_t count,
                uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 write 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 写入字节数。
 * @param timeout_ms 超时毫秒。
 * @return 写入结果，与系统调用 write 语义一致。
 */
ssize_t co_write(int fd, const void *buffer, size_t count,
                 uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 readv 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param iov iovec 数组。
 * @param iovcnt iovec 数量。
 * @param timeout_ms 超时毫秒。
 * @return 读取结果，与系统调用 readv 语义一致。
 */
ssize_t co_readv(int fd, const struct iovec *iov, int iovcnt,
                 uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 writev 包装。
 * @param fd 文件描述符。
 * @param iov iovec 数组。
 * @param iovcnt iovec 数量。
 * @param timeout_ms 超时毫秒。
 * @return 写入结果，与系统调用 writev 语义一致。
 */
ssize_t co_writev(int fd, const struct iovec *iov, int iovcnt,
                  uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 recv 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 读取字节数。
 * @param flags recv flags。
 * @param timeout_ms 超时毫秒。
 * @return 接收结果。
 */
ssize_t co_recv(int fd, void *buffer, size_t count, int flags,
                uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 recvn 包装，读取满 count 字节。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 目标读取字节数。
 * @param flags recv flags。
 * @param timeout_ms 超时毫秒。
 * @return 成功返回 count；对端关闭返回 0；失败返回 -1。
 */
ssize_t co_recvn(int fd, void *buffer, size_t count, int flags = 0,
                 uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 send 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 写入字节数。
 * @param flags send flags。
 * @param timeout_ms 超时毫秒。
 * @return 发送结果。
 */
ssize_t co_send(int fd, const void *buffer, size_t count, int flags,
                uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 recvfrom 包装。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 读取字节数。
 * @param flags recvfrom flags。
 * @param address 输出地址。
 * @param address_len 输出地址长度。
 * @param timeout_ms 超时毫秒。
 * @return 接收结果，与系统调用 recvfrom 语义一致。
 */
ssize_t co_recvfrom(int fd, void *buffer, size_t count, int flags,
                    struct sockaddr *address, socklen_t *address_len,
                    uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 sendto 包装。
 * @param fd 文件描述符。
 * @param buffer 缓冲区。
 * @param count 写入字节数。
 * @param flags sendto flags。
 * @param address 目标地址。
 * @param address_len 地址长度。
 * @param timeout_ms 超时毫秒。
 * @return 发送结果，与系统调用 sendto 语义一致。
 */
ssize_t co_sendto(int fd, const void *buffer, size_t count, int flags,
                  const struct sockaddr *address, socklen_t address_len,
                  uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 connect 包装。必须在协程中调用。
 * @param fd 文件描述符。
 * @param address 地址结构。
 * @param address_len 地址长度。
 * @param timeout_ms 超时毫秒。
 * @return 连接结果，与系统调用 connect 语义一致。
 */
int co_connect(int fd, const struct sockaddr *address, socklen_t address_len,
               uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 accept 包装，必须在协程中调用。
 * @param fd 监听 socket。
 * @param address 输出地址。
 * @param address_len 输出地址长度。
 * @param timeout_ms 超时毫秒。
 * @return accept 结果，与系统调用 accept 语义一致。
 */
int co_accept(int fd, struct sockaddr *address, socklen_t *address_len,
              uint32_t timeout_ms = kInfiniteTimeoutMs);

/**
 * @brief 协程友好的 accept4 包装，必须在协程中调用。
 * @param fd 监听 socket。
 * @param address 输出地址。
 * @param address_len 输出地址长度。
 * @param flags accept4 flags。
 * @param timeout_ms 超时毫秒。
 * @return accept4 结果，与系统调用 accept4 语义一致。
 */
int co_accept4(int fd, struct sockaddr *address, socklen_t *address_len,
               int flags, uint32_t timeout_ms = kInfiniteTimeoutMs);

} // namespace zco

#endif // ZCO_HOOK_H_
