#include "znet/socket.h"

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "zcoroutine/hook.h"
#include "zcoroutine/sched.h"
#include "znet/znet_logger.h"

namespace {

constexpr int kCoroutineRequiredErrno = EPERM;

// 统一检查：I/O 接口必须在协程上下文内调用。
bool require_coroutine_context(const char *func_name) {
    if (zcoroutine::in_coroutine()) {
        return true;
    }

    errno = kCoroutineRequiredErrno;
    ZNET_LOG_ERROR("{} must be called inside zcoroutine::go context",
                   func_name);
    return false;
}

// 用户传 0 表示无限等待；超大值钳制到框架支持范围内。
uint32_t normalize_timeout_ms(uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return zcoroutine::kInfiniteTimeoutMs;
    }
    if (timeout_ms >= static_cast<uint64_t>(zcoroutine::kInfiniteTimeoutMs)) {
        return zcoroutine::kInfiniteTimeoutMs - 1;
    }
    return static_cast<uint32_t>(timeout_ms);
}

} // namespace

namespace znet {

// 按 family/type/protocol 创建新 socket 并执行基础初始化。
Socket::Socket(int family, int type, int protocol)
    : sockfd_(-1), family_(family), type_(type), protocol_(protocol),
      is_connected_(false) {
    new_sock();
}

// 基于已有 fd 包装：探测属性并补齐统一初始化逻辑。
Socket::Socket(int sockfd) : sockfd_(sockfd), is_connected_(false) {
    socklen_t len = sizeof(family_);
    if (::getsockopt(sockfd_, SOL_SOCKET, SO_DOMAIN, &family_, &len) != 0) {
        family_ = AF_INET;
    }
    len = sizeof(type_);
    if (::getsockopt(sockfd_, SOL_SOCKET, SO_TYPE, &type_, &len) != 0) {
        type_ = SOCK_STREAM;
    }
    protocol_ = 0;

    if (!init_sock()) {
        const int fd = sockfd_;
        (void)::close(sockfd_);
        sockfd_ = -1;
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

    if (zcoroutine::co_bind(sockfd_, addr->sockaddr_ptr(),
                            addr->sockaddr_len()) != 0) {
        ZNET_LOG_ERROR("Socket::bind failed: fd={}, errno={}, error={}",
                       sockfd_, errno, strerror(errno));
        return false;
    }

    get_local_address();
    ZNET_LOG_INFO("Socket::bind success: fd={}, addr={}", sockfd_,
                  local_address_->to_string());
    return true;
}

// 进入监听态，适用于 TCP 服务端 socket。
bool Socket::listen(int backlog) {
    if (!is_valid()) {
        ZNET_LOG_ERROR("Socket::listen invalid socket");
        return false;
    }

    if (zcoroutine::co_listen(sockfd_, backlog) != 0) {
        ZNET_LOG_ERROR("Socket::listen failed: fd={}, errno={}, error={}",
                       sockfd_, errno, strerror(errno));
        return false;
    }

    ZNET_LOG_INFO("Socket::listen success: fd={}, backlog={}", sockfd_,
                  backlog);
    return true;
}

// 协程版 accept：直接复用 zcoroutine 的 co_accept/co_accept4 语义。
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
    int clientfd = -1;
    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);

#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    // 优先 accept4 一步设置 NONBLOCK/CLOEXEC，减少额外系统调用。
    clientfd = zcoroutine::co_accept4(
        sockfd_, reinterpret_cast<sockaddr *>(&addr), &len,
        SOCK_NONBLOCK | SOCK_CLOEXEC, effective_timeout_ms);
    if (clientfd == -1 && errno == ENOSYS) {
        clientfd =
            zcoroutine::co_accept(sockfd_, reinterpret_cast<sockaddr *>(&addr),
                                  &len, effective_timeout_ms);
    }
#else
    clientfd =
        zcoroutine::co_accept(sockfd_, reinterpret_cast<sockaddr *>(&addr),
                              &len, effective_timeout_ms);
#endif

    if (clientfd < 0) {
        if (errno == EBADF) {
            return nullptr;
        }
        ZNET_LOG_ERROR("Socket::accept failed: fd={}, errno={}, error={}",
                       sockfd_, errno, strerror(errno));
        return nullptr;
    }

    Socket::ptr client_sock = std::make_shared<Socket>(clientfd);
    ZNET_LOG_INFO("Socket::accept success: fd={}, client_fd={}", sockfd_,
                  clientfd);
    return client_sock;
}

// 协程版 connect：语义由 co_connect 统一定义。
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
    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);
    if (zcoroutine::co_connect(sockfd_, addr->sockaddr_ptr(),
                               addr->sockaddr_len(),
                               effective_timeout_ms) != 0) {
        ZNET_LOG_ERROR(
            "Socket::connect failed: fd={}, addr={}, errno={}, error={}",
            sockfd_, addr->to_string(), errno, strerror(errno));
        return false;
    }

    is_connected_ = true;
    get_local_address();
    ZNET_LOG_INFO("Socket::connect success: fd={}, remote={}, local={}",
                  sockfd_, remote_address_->to_string(),
                  local_address_->to_string());
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

// 协程友好的 close 封装，避免直接调用系统 close 破坏 hook 语义。
bool Socket::close() {
    if (!is_valid()) {
        return true;
    }

    is_connected_ = false;
    if (zcoroutine::co_close(sockfd_) != 0) {
        ZNET_LOG_ERROR("Socket::close failed: fd={}, errno={}, error={}",
                       sockfd_, errno, strerror(errno));
        return false;
    }
    sockfd_ = -1;
    return true;
}

// 半关闭写端，常用于优雅关闭流程。
bool Socket::shutdown_write() {
    if (!is_valid()) {
        return true;
    }

    if (zcoroutine::co_shutdown(sockfd_, 'w') != 0) {
        const int err = errno;
        ZNET_LOG_ERROR(
            "Socket::shutdown_write failed: fd={}, errno={}, error={}", sockfd_,
            err, strerror(err));
        return false;
    }

    is_connected_ = false;
    ZNET_LOG_DEBUG("Socket::shutdown_write success: fd={}", sockfd_);
    return true;
}

// 协程版 send：语义由 co_send 统一定义（发送满 length 或返回失败）。
ssize_t Socket::send(const void *buffer, size_t length, int flags,
                     uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::send")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);
    const ssize_t n = zcoroutine::co_send(sockfd_, buffer, length, flags,
                                          effective_timeout_ms);
    return n;
}

// 协程版 recv：语义由 co_recv 统一定义。
ssize_t Socket::recv(void *buffer, size_t length, int flags,
                     uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::recv")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);
    const ssize_t n = zcoroutine::co_recv(sockfd_, buffer, length, flags,
                                          effective_timeout_ms);
    return n;
}

// UDP 发送封装，语义由 co_sendto 统一定义。
ssize_t Socket::send_to(const void *buffer, size_t length,
                        const Address::ptr to, int flags, uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::send_to")) {
        return -1;
    }

    if (!is_valid() || !to) {
        errno = EINVAL;
        return -1;
    }

    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);
    const ssize_t n = zcoroutine::co_sendto(
        sockfd_, buffer, length, flags, to->sockaddr_ptr(), to->sockaddr_len(),
        effective_timeout_ms);
    return n;
}

// UDP 接收封装，成功时可选择输出来源地址。
ssize_t Socket::recv_from(void *buffer, size_t length, Address::ptr from,
                          int flags, uint64_t timeout_ms) {
    if (!require_coroutine_context("Socket::recv_from")) {
        return -1;
    }

    if (!is_valid()) {
        return -1;
    }

    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    const uint32_t effective_timeout_ms = normalize_timeout_ms(timeout_ms);
    const ssize_t ret = zcoroutine::co_recvfrom(
        sockfd_, buffer, length, flags, reinterpret_cast<sockaddr *>(&addr),
        &len, effective_timeout_ms);
    if (ret >= 0 && from) {
        // 注意：from 为值传递，此赋值不会回传到调用方。
        from = Address::create(reinterpret_cast<sockaddr *>(&addr), len);
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
        zcoroutine::co_set_nonblock(sockfd_);
        const int flags = ::fcntl(sockfd_, F_GETFL, 0);
        if (flags == -1 || (flags & O_NONBLOCK) == 0) {
            ZNET_LOG_ERROR(
                "Socket::set_non_blocking verify non-blocking failed: fd={}",
                sockfd_);
            return false;
        }
        return true;
    }

    int flags = ::fcntl(sockfd_, F_GETFL, 0);
    if (flags == -1) {
        ZNET_LOG_ERROR("Socket::set_non_blocking fcntl F_GETFL failed: fd={}",
                       sockfd_);
        return false;
    }

    flags &= ~O_NONBLOCK;

    if (::fcntl(sockfd_, F_SETFL, flags) == -1) {
        ZNET_LOG_ERROR("Socket::set_non_blocking fcntl F_SETFL failed: fd={}",
                       sockfd_);
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
    if (::getsockname(sockfd_, reinterpret_cast<sockaddr *>(&addr), &len) !=
        0) {
        ZNET_LOG_ERROR("Socket::get_local_address failed: fd={}", sockfd_);
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
    if (::getpeername(sockfd_, reinterpret_cast<sockaddr *>(&addr), &len) !=
        0) {
        ZNET_LOG_ERROR("Socket::get_remote_address failed: fd={}", sockfd_);
        return nullptr;
    }

    remote_address_ = Address::create(reinterpret_cast<sockaddr *>(&addr), len);
    return remote_address_;
}

// 读取 SO_ERROR 作为最近一次 socket 错误状态。
int Socket::get_error() {
    int error = 0;
    socklen_t len = sizeof(error);
    if (!get_option(SOL_SOCKET, SO_ERROR, &error)) {
        return -1;
    }
    (void)len;
    return error;
}

// 新建/接管 fd 后的统一初始化：非阻塞 + 常用选项。
bool Socket::init_sock() {
    if (!set_non_blocking(true)) {
        ZNET_LOG_ERROR("Socket::init_sock failed to set non-blocking: fd={}",
                       sockfd_);
        return false;
    }

    zcoroutine::co_set_cloexec(sockfd_);

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
    sockfd_ = zcoroutine::co_socket(family_, type_, protocol_);
    if (sockfd_ == -1) {
        ZNET_LOG_ERROR(
            "Socket::new_sock failed: family={}, type={}, protocol={}, "
            "errno={}, error={}",
            family_, type_, protocol_, errno, strerror(errno));
        return false;
    }

    if (!init_sock()) {
        const int fd = sockfd_;
        (void)::close(sockfd_);
        sockfd_ = -1;
        ZNET_LOG_ERROR("Socket::new_sock init failed: fd={}", fd);
        return false;
    }

    ZNET_LOG_DEBUG("Socket::new_sock success: fd={}, family={}, type={}",
                   sockfd_, family_, type_);
    return true;
}

} // namespace znet
