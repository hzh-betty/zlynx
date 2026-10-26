#ifndef ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_SOCKET_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_SOCKET_H_

#include "zco/deadline.h"
#include "zco/io/descriptor.h"
#include "znet/transport/byte_stream.h"
#include <cerrno>
#include <memory>
#include <string_view>

namespace znet {
enum class SocketKind { stream, datagram };

struct Datagram {
    size_t bytes;
    Endpoint source; 
    bool truncated; // 数据包被截断
};

/**
 * @brief 独占非阻塞套接字；移动对象时不得有尚未结束的操作。
 * accept/connect/传输操作要求在 zco 任务中调用；close 可由任意线程调用，
 * 并撤销未完成等待；原生句柄仅供借用，不得手动关闭。
 */
class Socket {
  public:
    static Result<Socket> create(int family, SocketKind kind);

    /**
     * @brief 校验并接管 fd 的所有权，失败时也负责关闭。
     */
    static Result<Socket> adopt(int fd);
    Socket(Socket &&) noexcept = default;
    Socket &operator=(Socket &&) noexcept = default;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    int native_handle() const { return descriptor_.native_handle(); }

    const zco::io::Descriptor &descriptor() const { return descriptor_; }

    Result<void> bind(const Endpoint &endpoint);
    Result<void> listen(int backlog = SOMAXCONN);
    Result<Socket> accept(zco::Deadline deadline = {});
    Result<void> connect(const Endpoint &endpoint, zco::Deadline deadline = {});
    Transfer read_some(void *bytes, size_t size, zco::Deadline deadline = {});
    Transfer write_some(const void *bytes, size_t size,
                        zco::Deadline deadline = {});
    Transfer send_to(std::string_view bytes, const Endpoint &destination,
                     zco::Deadline deadline = {});
    Result<Datagram> receive_from(void *bytes, size_t size,
                                  zco::Deadline deadline = {});
    Result<void> shutdown_write();
    Result<void> close();
    Result<Endpoint> local_endpoint() const;
    Result<Endpoint> remote_endpoint() const;

    template <class T>
    Result<void> set_option(int level, int name, const T &value) {
        auto fd = descriptor_.borrow();
        if (::setsockopt(fd.fd(), level, name, &value, sizeof(value)) < 0)
            return io_error("set socket option", errno);
        return {};
    }

    template <class T> Result<T> option(int level, int name) const {
        T value{};
        socklen_t size = sizeof(value);
        auto fd = descriptor_.borrow();
        if (::getsockopt(fd.fd(), level, name, &value, &size) < 0)
            return io_error("get socket option", errno);
        return value;
    }

  private:
    Socket(zco::io::Descriptor descriptor, int family, SocketKind kind)
        : descriptor_(std::move(descriptor)), family_(family), kind_(kind) {}

    /**
     * @brief 调用方须提供已校验的元数据，以及设置 NONBLOCK/CLOEXEC 的描述符。
     */
    static Result<Socket> from_descriptor(zco::io::Descriptor descriptor,
                                          int family, SocketKind kind);
    Result<Endpoint> endpoint(bool remote) const;
    zco::io::Descriptor descriptor_;
    int family_;
    SocketKind kind_;
};

Result<std::unique_ptr<ByteStream>> make_tcp_stream(Socket socket);
} // namespace znet

#endif
