#include "znet/transport/socket.h"
#include "zco/coroutine.h"
#include "zco/io/operations.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

namespace znet {
namespace {
Error invalid(std::string operation) {
    return make_error(ErrorKind::validation, std::errc::invalid_argument,
                      std::move(operation));
}

template <class F>
Result<size_t> retry(const Socket &socket, zco::io::Interest interest,
                     zco::Deadline deadline, const char *operation, F syscall) {
    if (!zco::in_coroutine())
        return make_error(ErrorKind::runtime,
                          std::errc::operation_not_permitted, operation,
                          "IO requires a runtime task");
    for (;;) {
        if (deadline.expired(zco::Deadline::Clock::now()))
            return make_error(ErrorKind::runtime, std::errc::timed_out,
                              operation);
        ssize_t count;
        int error;
        {
            auto fd = socket.descriptor().borrow();
            if (fd.fd() < 0)
                return io_error(operation, EBADF);
            count = syscall(fd.fd());
            error = errno;
        }
        if (count >= 0)
            return static_cast<size_t>(count);
        if (error == EINTR)
            continue;
        if (error != EAGAIN && error != EWOULDBLOCK)
            return io_error(operation, error);
        auto ready =
            zco::io::wait_ready(socket.descriptor(), interest, deadline);
        if (!ready)
            return runtime_error(operation, ready.error());
    }
}
} // namespace

Result<Socket> Socket::create(int family, SocketKind kind) {
    if (family != AF_INET && family != AF_INET6 && family != AF_UNIX)
        return invalid("create socket");
    if (kind != SocketKind::stream && kind != SocketKind::datagram)
        return invalid("create socket");
    const int type = kind == SocketKind::stream ? SOCK_STREAM : SOCK_DGRAM;
    const int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return io_error("create socket", errno);
    return adopt(fd);
}

Result<Socket> Socket::adopt(int fd) {
    if (fd < 0)
        return invalid("adopt socket");
    zco::io::Descriptor descriptor(fd);
    int family = 0, type = 0;
    socklen_t size = sizeof(int);
    if (::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &size) < 0 ||
        ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) < 0)
        return io_error("adopt socket", errno);
    if ((family != AF_INET && family != AF_INET6 && family != AF_UNIX) ||
        (type != SOCK_STREAM && type != SOCK_DGRAM))
        return invalid("adopt socket");
    const int status = ::fcntl(fd, F_GETFL);
    const int flags = ::fcntl(fd, F_GETFD);
    if (status < 0 || flags < 0 ||
        ::fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0 ||
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return io_error("configure socket", errno);
    Socket socket(std::move(descriptor), family,
                  type == SOCK_STREAM ? SocketKind::stream
                                      : SocketKind::datagram);
    if (type == SOCK_STREAM && family != AF_UNIX) {
        auto configured = socket.set_option(IPPROTO_TCP, TCP_NODELAY, 1);
        if (!configured)
            return configured.error();
    }
    return socket;
}

Result<void> Socket::bind(const Endpoint &endpoint) {
    if (endpoint.family() != family_)
        return invalid("bind: address family mismatch");
    auto fd = descriptor_.borrow();
    if (::bind(fd.fd(), endpoint.native_address(), endpoint.native_size()) < 0)
        return io_error("bind", errno);
    return {};
}

Result<void> Socket::listen(int backlog) {
    if (kind_ != SocketKind::stream || backlog <= 0)
        return invalid("listen");
    auto fd = descriptor_.borrow();
    if (::listen(fd.fd(), backlog) < 0)
        return io_error("listen", errno);
    return {};
}

Result<Socket> Socket::accept(zco::Deadline deadline) {
    if (kind_ != SocketKind::stream)
        return invalid("accept");
    auto accepted =
        retry(*this, zco::io::Interest::read, deadline, "accept", [](int fd) {
            return ::accept4(fd, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        });
    if (!accepted)
        return accepted.error();
    return adopt(static_cast<int>(accepted.value()));
}

Result<void> Socket::connect(const Endpoint &endpoint, zco::Deadline deadline) {
    if (endpoint.family() != family_)
        return invalid("connect: address family mismatch");
    if (!zco::in_coroutine())
        return make_error(ErrorKind::runtime,
                          std::errc::operation_not_permitted, "connect",
                          "IO requires a runtime task");
    if (deadline.expired(zco::Deadline::Clock::now()))
        return make_error(ErrorKind::runtime, std::errc::timed_out, "connect");
    int rc, error;
    {
        auto fd = descriptor_.borrow();
        rc = ::connect(fd.fd(), endpoint.native_address(),
                       endpoint.native_size());
        error = errno;
    }
    if (rc == 0 || (rc < 0 && error == EISCONN))
        return {};
    if (error != EINPROGRESS && error != EALREADY && error != EINTR)
        return io_error("connect", error);
    auto ready =
        zco::io::wait_ready(descriptor_, zco::io::Interest::write, deadline);
    if (!ready)
        return runtime_error("connect", ready.error());
    auto status = option<int>(SOL_SOCKET, SO_ERROR);
    if (!status)
        return status.error();
    if (status.value())
        return io_error("connect", status.value());
    return {};
}

Transfer Socket::read_some(void *bytes, size_t size, zco::Deadline deadline) {
    if (kind_ != SocketKind::stream || (!bytes && size))
        return {0, invalid("read"), false};
    if (!size)
        return {};
    auto count = retry(*this, zco::io::Interest::read, deadline, "read",
                       [&](int fd) { return ::recv(fd, bytes, size, 0); });
    if (!count)
        return {0, count.error(), false};
    return {count.value(), {}, count.value() == 0};
}

Transfer Socket::write_some(const void *bytes, size_t size,
                            zco::Deadline deadline) {
    if (kind_ != SocketKind::stream || (!bytes && size))
        return {0, invalid("write"), false};
    if (!size)
        return {};
    auto count =
        retry(*this, zco::io::Interest::write, deadline, "write",
              [&](int fd) { return ::send(fd, bytes, size, MSG_NOSIGNAL); });
    if (!count)
        return {0, count.error(), false};
    return {count.value(), {}, false};
}

Transfer Socket::send_to(std::string_view bytes, const Endpoint &destination,
                         zco::Deadline deadline) {
    if (kind_ != SocketKind::datagram || destination.family() != family_)
        return {0, invalid("send datagram"), false};
    auto count =
        retry(*this, zco::io::Interest::write, deadline, "send datagram",
              [&](int fd) {
                  return ::sendto(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL,
                                  destination.native_address(),
                                  destination.native_size());
              });
    if (!count)
        return {0, count.error(), false};
    return {count.value(), {}, false};
}

Result<Datagram> Socket::receive_from(void *bytes, size_t size,
                                      zco::Deadline deadline) {
    if (kind_ != SocketKind::datagram || (!bytes && size))
        return invalid("receive datagram");
    sockaddr_storage address{};
    socklen_t address_size = sizeof(address);
    auto count =
        retry(*this, zco::io::Interest::read, deadline, "receive datagram",
              [&](int fd) {
                  address_size = sizeof(address);
                  return ::recvfrom(fd, bytes, size, MSG_TRUNC,
                                    reinterpret_cast<sockaddr *>(&address),
                                    &address_size);
              });
    if (!count)
        return count.error();
    // Unix datagrams may come from an unbound socket, in which case the
    // kernel supplies no sockaddr. The payload is still a valid message.
    auto source =
        family_ == AF_UNIX && address_size == 0
            ? Endpoint::unix_domain({})
            : Endpoint::from_native(reinterpret_cast<sockaddr *>(&address),
                                    address_size);
    if (!source)
        return source.error();
    return Datagram{std::min(size, count.value()), std::move(source).value(),
                    count.value() > size};
}

Result<void> Socket::shutdown_write() {
    auto fd = descriptor_.borrow();
    if (fd.fd() < 0)
        return {};
    if (::shutdown(fd.fd(), SHUT_WR) < 0)
        return io_error("shutdown write", errno);
    return {};
}

Result<void> Socket::close() {
    auto closed = descriptor_.close();
    if (!closed)
        return io_error("close socket", closed.error().value());
    return {};
}

Result<Endpoint> Socket::endpoint(bool remote) const {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    {
        auto fd = descriptor_.borrow();
        const int rc =
            remote
                ? ::getpeername(fd.fd(), reinterpret_cast<sockaddr *>(&address),
                                &size)
                : ::getsockname(fd.fd(), reinterpret_cast<sockaddr *>(&address),
                                &size);
        if (rc < 0)
            return io_error(remote ? "remote endpoint" : "local endpoint",
                            errno);
    }
    return Endpoint::from_native(reinterpret_cast<sockaddr *>(&address), size);
}

Result<Endpoint> Socket::local_endpoint() const { return endpoint(false); }

Result<Endpoint> Socket::remote_endpoint() const { return endpoint(true); }
} // namespace znet
