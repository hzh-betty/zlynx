/**
 * @file socket.cc
 * @brief socket 实现。
 * @author hzh-betty
 */

#include "znet/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>

#include "zco/coroutine.h"
#include "zco/io/operations.h"

#include "znet/znet_logger.h"

namespace {

constexpr int kCoroutineRequiredErrno = EPERM;

// 统一检查：I/O 接口必须在协程上下文内调用。
bool require_coroutine_context(const char *func_name) {
    if (zco::in_coroutine()) {
        return true;
    }

    errno = kCoroutineRequiredErrno;
    ZNET_LOG_ERROR("{} must be called inside a runtime task", func_name);
    return false;
}

bool operation_deadline(const zco::io::Descriptor &descriptor,
                        uint64_t timeout_ms, bool reading,
                        zco::Deadline &deadline) {
    if (timeout_ms) {
        deadline = zco::Deadline::after(std::chrono::milliseconds(timeout_ms));
        return true;
    }
    auto result = zco::io::socket_deadline(descriptor, reading);
    if (!result) {
        errno = result.error().value();
        return false;
    }
    deadline = result.value();
    return true;
}

template <class F>
ssize_t retry_socket(const zco::io::Descriptor &descriptor,
                     zco::io::Interest interest, zco::Deadline deadline, F fn) {
    while (true) {
        ssize_t result;
        int error;
        {
            auto borrowed = descriptor.borrow();
            int flags = ::fcntl(borrowed.fd(), F_GETFL);
            if (flags < 0)
                return -1;
            if (!(flags & O_NONBLOCK)) {
                errno = EPERM;
                return -1;
            }
            result = fn(borrowed.fd());
            error = errno;
        }
        if (result >= 0)
            return result;
        if (error == EINTR) {
            if (deadline.expired(zco::Deadline::Clock::now())) {
                errno = ETIMEDOUT;
                return -1;
            }
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            errno = error;
            return -1;
        }
        auto ready = zco::io::wait_ready(descriptor, interest, deadline);
        if (!ready) {
            errno = ready.error().value();
            return -1;
        }
    }
}

} // namespace

namespace znet {

// 按 family/type/protocol 创建新 socket 并执行基础初始化。
Socket::Socket(int family, int type, int protocol)
    : descriptor_(), family_(family), type_(type), protocol_(protocol) {
    new_sock();
}

// 基于已有 fd 包装：探测属性并补齐统一初始化逻辑。
Socket::Socket(int sockfd) : descriptor_() {
    if (sockfd >= 0)
        descriptor_ = zco::io::Descriptor(sockfd);
    socklen_t len = sizeof(family_);
    if (::getsockopt(fd(), SOL_SOCKET, SO_DOMAIN, &family_, &len) != 0) {
        family_ = AF_INET;
    }
    len = sizeof(type_);
    if (::getsockopt(fd(), SOL_SOCKET, SO_TYPE, &type_, &len) != 0) {
        type_ = SOCK_STREAM;
    }
    protocol_ = 0;

    if (!init_sock()) {
        const int fd = this->fd();
        (void)descriptor_.close();
        ZNET_LOG_ERROR(
            "Socket::Socket existing fd init failed: fd={}, errno={}, error={}",
            fd, errno, strerror(errno));
    }
}

Socket::~Socket() { close(); }

// 便捷工厂：IPv4 TCP。
Socket::ptr Socket::create_tcp() {
    return std::make_shared<Socket>(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

// 便捷工厂：根据地址族创建 TCP。
Socket::ptr Socket::create_tcp(Address::ptr address) {
    return std::make_shared<Socket>(address->family(), SOCK_STREAM,
                                    IPPROTO_TCP);
}

// 便捷工厂：IPv6 TCP。
Socket::ptr Socket::create_tcp_v6() {
    return std::make_shared<Socket>(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
}

// 便捷工厂：IPv4 UDP。
Socket::ptr Socket::create_udp() {
    return std::make_shared<Socket>(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

// 便捷工厂：IPv6 UDP。
Socket::ptr Socket::create_udp_v6() {
    return std::make_shared<Socket>(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
}

// 绑定本地地址，并缓存本端地址用于后续日志和查询。
bool Socket::bind(const Address::ptr addr) {
    if (!is_valid()) {
        ZNET_LOG_ERROR("Socket::bind invalid socket");
        return false;
    }

    if (addr->family() != family_) {
        ZNET_LOG_ERROR(
            "Socket::bind address family mismatch: socket={}, addr={}", family_,
            addr->family());
        return false;
    }

    if (::bind(fd(), addr->sockaddr_ptr(), addr->sockaddr_len()) != 0) {
        ZNET_LOG_ERROR("Socket::bind failed: fd={}, errno={}, error={}", fd(),
                       errno, strerror(errno));
        return false;
    }

    get_local_address();
    ZNET_LOG_INFO("Socket::bind success: fd={}, addr={}", fd(),
                  local_address_->to_string());
    return true;
}

// 进入监听态，适用于 TCP 服务端 socket。
bool Socket::listen(int backlog) {
    if (!is_valid()) {
        ZNET_LOG_ERROR("Socket::listen invalid socket");
        return false;
    }

    if (::listen(fd(), backlog) != 0) {
        ZNET_LOG_ERROR("Socket::listen failed: fd={}, errno={}, error={}", fd(),
                       errno, strerror(errno));
        return false;
    }

    ZNET_LOG_INFO("Socket::listen success: fd={}, backlog={}", fd(), backlog);
    return true;
}

// 网络接受策略使用同一绝对截止时间。
Socket::ptr Socket::accept(uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::accept")) {
        return nullptr;
    }

    if (!is_valid()) {
        ZNET_LOG_ERROR("Socket::accept invalid socket");
        return nullptr;
    }

    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, true, deadline))
        return nullptr;
    int clientfd = static_cast<int>(retry_socket(
        descriptor_, zco::io::Interest::read, deadline, [&](int fd) {
            return ::accept4(fd, reinterpret_cast<sockaddr *>(&addr), &len,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        }));

    if (clientfd < 0) {
        if (errno == EBADF) {
            return nullptr;
        }
        ZNET_LOG_ERROR("Socket::accept failed: fd={}, errno={}, error={}", fd(),
                       errno, strerror(errno));
        return nullptr;
    }

    Socket::ptr client_sock = std::make_shared<Socket>(clientfd);
    ZNET_LOG_INFO("Socket::accept success: fd={}, client_fd={}", fd(),
                  clientfd);
    return client_sock;
}

// 连接策略归网络层；运行时只负责就绪等待。
bool Socket::connect(const Address::ptr addr, uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::connect")) {
        return false;
    }

    if (!is_valid()) {
        ZNET_LOG_ERROR("Socket::connect invalid socket");
        return false;
    }

    if (addr->family() != family_) {
        ZNET_LOG_ERROR(
            "Socket::connect address family mismatch: socket={}, addr={}",
            family_, addr->family());
        return false;
    }

    remote_address_ = addr;
    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, false, deadline))
        return false;
    int result;
    {
        auto borrowed = descriptor_.borrow();
        result = ::connect(borrowed.fd(), addr->sockaddr_ptr(),
                           addr->sockaddr_len());
    }
    if (result < 0 && errno != EISCONN) {
        if (errno != EINPROGRESS && errno != EALREADY)
            return false;
        auto ready = zco::io::wait_ready(descriptor_, zco::io::Interest::write,
                                         deadline);
        if (!ready) {
            errno = ready.error().value();
            return false;
        }
        int error = get_error();
        if (error) {
            errno = error;
            return false;
        }
    }

    get_local_address();
    ZNET_LOG_INFO("Socket::connect success: fd={}, remote={}, local={}", fd(),
                  remote_address_->to_string(), local_address_->to_string());
    return true;
}

// 复用最近一次 connect 的远端地址执行重连。
bool Socket::reconnect(uint64_t timeout_ms) {
    if (!remote_address_) {
        ZNET_LOG_ERROR("Socket::reconnect no remote address");
        return false;
    }
    local_address_.reset();
    return connect(remote_address_, timeout_ms);
}

// Descriptor 关闭前撤销注册并完成等待。
bool Socket::close() {
    if (!is_valid()) {
        return true;
    }

    auto result = descriptor_.close();
    if (!result)
        errno = result.error().value();
    return static_cast<bool>(result);
}

// 半关闭写端，常用于优雅关闭流程。
bool Socket::shutdown_write() {
    if (!is_valid()) {
        return true;
    }

    if (::shutdown(fd(), SHUT_WR) != 0) {
        const int err = errno;
        ZNET_LOG_ERROR(
            "Socket::shutdown_write failed: fd={}, errno={}, error={}", fd(),
            err, strerror(err));
        return false;
    }

    ZNET_LOG_DEBUG("Socket::shutdown_write success: fd={}", fd());
    return true;
}

// 流发送保留部分进度；数据报仅执行一次发送。
ssize_t Socket::send(const void *buffer, size_t length, int flags,
                     uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::send")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, false, deadline))
        return -1;
    size_t sent = 0;
    do {
        if (sent && deadline.expired(zco::Deadline::Clock::now())) {
            errno = ETIMEDOUT;
            return static_cast<ssize_t>(sent);
        }
        auto count = retry_socket(
            descriptor_, zco::io::Interest::write, deadline, [&](int fd) {
                return ::send(fd, static_cast<const char *>(buffer) + sent,
                              length - sent, flags | MSG_NOSIGNAL);
            });
        if (count < 0)
            return sent ? static_cast<ssize_t>(sent) : -1;
        sent += static_cast<size_t>(count);
        if (type_ == SOCK_DGRAM || !count)
            break;
    } while (sent < length);
    return static_cast<ssize_t>(sent);
}

// 接收使用一次操作的截止时间。
ssize_t Socket::recv(void *buffer, size_t length, int flags,
                     uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::recv")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, true, deadline))
        return -1;
    return retry_socket(
        descriptor_, zco::io::Interest::read, deadline,
        [&](int fd) { return ::recv(fd, buffer, length, flags); });
}

// 数据报发送保留零长度消息语义。
ssize_t Socket::send_to(const void *buffer, size_t length,
                        const Address::ptr to, int flags, uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::send_to")) {
        return -1;
    }

    if (!is_valid() || !to) {
        errno = EINVAL;
        return -1;
    }

    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, false, deadline))
        return -1;
    return retry_socket(
        descriptor_, zco::io::Interest::write, deadline, [&](int fd) {
            return ::sendto(fd, buffer, length, flags | MSG_NOSIGNAL,
                            to->sockaddr_ptr(), to->sockaddr_len());
        });
}

// UDP 接收封装，成功时可选择输出来源地址。
ssize_t Socket::recv_from(void *buffer, size_t length, Address::ptr *from,
                          int flags, uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::recv_from")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    zco::Deadline deadline;
    if (!operation_deadline(descriptor_, timeout_ms, true, deadline))
        return -1;
    const ssize_t ret = retry_socket(
        descriptor_, zco::io::Interest::read, deadline, [&](int fd) {
            return ::recvfrom(fd, buffer, length, flags,
                              reinterpret_cast<sockaddr *>(&addr), &len);
        });
    if (ret >= 0 && from) {
        *from = Address::create(reinterpret_cast<sockaddr *>(&addr), len);
    }
    return ret;
}

// 毫秒超时转换为 timeval 并写入内核 SO_SNDTIMEO。
bool Socket::set_send_timeout(uint64_t timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return set_option(SOL_SOCKET, SO_SNDTIMEO, tv);
}

// 毫秒超时转换为 timeval 并写入内核 SO_RCVTIMEO。
bool Socket::set_recv_timeout(uint64_t timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return set_option(SOL_SOCKET, SO_RCVTIMEO, tv);
}

bool Socket::set_tcp_nodelay(bool on) {
    int optval = on ? 1 : 0;
    return set_option(IPPROTO_TCP, TCP_NODELAY, optval);
}

bool Socket::set_reuse_addr(bool on) {
    int optval = on ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEADDR, optval);
}

bool Socket::set_reuse_port(bool on) {
    int optval = on ? 1 : 0;
    return set_option(SOL_SOCKET, SO_REUSEPORT, optval);
}

bool Socket::set_keep_alive(bool on) {
    int optval = on ? 1 : 0;
    return set_option(SOL_SOCKET, SO_KEEPALIVE, optval);
}

// 切换 fd 的 O_NONBLOCK 标志。
bool Socket::set_non_blocking(bool on) {
    if (on) {
        const int current = ::fcntl(fd(), F_GETFL, 0);
        if (current < 0 || ::fcntl(fd(), F_SETFL, current | O_NONBLOCK) < 0)
            return false;
        const int flags = ::fcntl(fd(), F_GETFL, 0);
        if (flags == -1 || (flags & O_NONBLOCK) == 0) {
            ZNET_LOG_ERROR(
                "Socket::set_non_blocking verify non-blocking failed: fd={}",
                fd());
            return false;
        }
        return true;
    }

    int flags = ::fcntl(fd(), F_GETFL, 0);
    if (flags == -1) {
        ZNET_LOG_ERROR("Socket::set_non_blocking fcntl F_GETFL failed: fd={}",
                       fd());
        return false;
    }

    flags &= ~O_NONBLOCK;

    if (::fcntl(fd(), F_SETFL, flags) == -1) {
        ZNET_LOG_ERROR("Socket::set_non_blocking fcntl F_SETFL failed: fd={}",
                       fd());
        return false;
    }

    return true;
}

// 查询并缓存本地地址，后续重复调用直接返回缓存。
Address::ptr Socket::get_local_address() {
    if (local_address_) {
        return local_address_;
    }

    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd(), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        ZNET_LOG_ERROR("Socket::get_local_address failed: fd={}", fd());
        return nullptr;
    }

    local_address_ = Address::create(reinterpret_cast<sockaddr *>(&addr), len);
    return local_address_;
}

// 查询并缓存远端地址，后续重复调用直接返回缓存。
Address::ptr Socket::get_remote_address() {
    if (remote_address_) {
        return remote_address_;
    }

    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (::getpeername(fd(), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        ZNET_LOG_ERROR("Socket::get_remote_address failed: fd={}", fd());
        return nullptr;
    }

    remote_address_ = Address::create(reinterpret_cast<sockaddr *>(&addr), len);
    return remote_address_;
}

// 读取 SO_ERROR 作为最近一次 socket 错误状态。
int Socket::get_error() {
    int error = 0;
    if (!get_option(SOL_SOCKET, SO_ERROR, &error)) {
        return -1;
    }
    return error;
}

// 新建/接管 fd 后的统一初始化：非阻塞 + 常用选项。
bool Socket::init_sock() {
    if (!set_non_blocking(true)) {
        ZNET_LOG_ERROR("Socket::init_sock failed to set non-blocking: fd={}",
                       fd());
        return false;
    }

    if (::fcntl(fd(), F_SETFD, FD_CLOEXEC) < 0)
        return false;

    // REUSEADDR 可降低服务重启时端口占用带来的 bind 失败概率。
    (void)set_reuse_addr(true);
    if (type_ == SOCK_STREAM) {
        // TCP 默认关闭 Nagle，减少小包交互延迟。
        (void)set_tcp_nodelay(true);
    }
    return true;
}

// 创建底层 fd 并完成初始化，失败时保证资源回收干净。
bool Socket::new_sock() {
    const int created =
        ::socket(family_, type_ | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol_);
    if (created >= 0)
        descriptor_ = zco::io::Descriptor(created);
    if (fd() == -1) {
        ZNET_LOG_ERROR(
            "Socket::new_sock failed: family={}, type={}, protocol={}, "
            "errno={}, error={}",
            family_, type_, protocol_, errno, strerror(errno));
        return false;
    }

    if (!init_sock()) {
        const int fd = this->fd();
        (void)descriptor_.close();
        ZNET_LOG_ERROR("Socket::new_sock init failed: fd={}", fd);
        return false;
    }

    ZNET_LOG_DEBUG("Socket::new_sock success: fd={}, family={}, type={}", fd(),
                   family_, type_);
    return true;
}

} // namespace znet
