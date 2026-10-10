#pragma once

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
    bool truncated;
};

// Unique, nonblocking OS socket. Movement requires no outstanding operations.
// accept/connect/transfer require a zco task; close may run on any thread and
// revokes pending waits. Native handles are borrowed, never manually closed.
class Socket {
  public:
    static Result<Socket> create(int family, SocketKind kind);
    static Result<Socket> adopt(int fd); // Consumes fd, including on failure.
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

    // Callers provide validated metadata and NONBLOCK/CLOEXEC descriptors.
    static Result<Socket> from_descriptor(zco::io::Descriptor descriptor,
                                          int family, SocketKind kind);
    Result<Endpoint> endpoint(bool remote) const;
    zco::io::Descriptor descriptor_;
    int family_;
    SocketKind kind_;
};

Result<std::unique_ptr<ByteStream>> make_tcp_stream(Socket socket);
} // namespace znet
