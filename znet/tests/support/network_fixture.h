#pragma once

#include "znet/server/connection.h"
#include "znet/transport/socket.h"
#include <algorithm>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <unistd.h>

namespace znet::test {
inline Socket adopt(int fd) {
    auto result = Socket::adopt(fd);
    if (!result)
        throw std::runtime_error(result.error().message());
    return std::move(result).value();
}

inline Connection::ptr connection(Socket socket, zco::Runtime &runtime,
                                  std::chrono::milliseconds timeout = {}) {
    auto stream = make_tcp_stream(std::move(socket));
    if (!stream)
        throw std::runtime_error(stream.error().message());
    auto result = std::make_shared<Connection>(std::move(stream).value(),
                                               runtime.executor(0), timeout);
    auto started = result->start();
    if (!started)
        throw std::runtime_error(started.error().message());
    return result;
}

struct NetworkPair {
    explicit NetworkPair(std::chrono::milliseconds timeout = {})
        : runtime(zco::RuntimeOptions{1}) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            throw std::runtime_error("socketpair");
        peer.emplace(adopt(fds[1]));
        local = connection(adopt(fds[0]), runtime, timeout);
    }

    ~NetworkPair() { (void)local->close(); }

    zco::Runtime runtime;
    std::optional<Socket> peer;
    Connection::ptr local;
};

inline std::string receive(int fd, size_t size) {
    std::string result;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (result.size() < size && std::chrono::steady_clock::now() < end) {
        pollfd ready{fd, POLLIN, 0};
        if (::poll(&ready, 1, 20) <= 0)
            continue;
        char bytes[4096];
        const auto n =
            ::recv(fd, bytes, std::min(sizeof(bytes), size - result.size()),
                   MSG_DONTWAIT);
        if (n <= 0)
            break;
        result.append(bytes, static_cast<size_t>(n));
    }
    return result;
}

inline Socket connect_to(const Endpoint &endpoint) {
    int fd = ::socket(endpoint.family(), SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error("client socket");
    if (::connect(fd, endpoint.native_address(), endpoint.native_size()) < 0) {
        ::close(fd);
        throw std::runtime_error("client connect");
    }
    return adopt(fd);
}
} // namespace znet::test
