#ifndef ZLYNX_ZHTTP_TESTS_SUPPORT_SERVER_FIXTURE_H_
#define ZLYNX_ZHTTP_TESTS_SUPPORT_SERVER_FIXTURE_H_

#include "zco/deadline.h"
#include "zhttp/http_server.h"
#include "zhttp/websocket/websocket_handler.h"
#include "znet/transport/socket.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <poll.h>
#include <string>
#include <string_view>

namespace zhttp::test {

class ScopedServer {
  public:
    explicit ScopedServer(HttpServer::ptr server)
        : server_(std::move(server)) {}

    ~ScopedServer() { server_->stop(); }

  private:
    HttpServer::ptr server_;
};

inline HttpServer::ptr make_server(size_t threads = 1) {
    return std::make_shared<HttpServer>(
        znet::Endpoint::ipv4("127.0.0.1", 0).value(),
        zco::RuntimeOptions{threads});
}

inline void register_websocket(HttpServer &server, const std::string &path,
                               WebSocketCallbacks callbacks,
                               WebSocketOptions options = {}) {
    server.router().get(path,
                        [callbacks = std::move(callbacks),
                         options = std::move(options)](HttpContext &context) {
                            context.upgrade_to_websocket(callbacks, options);
                        });
}

inline std::error_code wait_ready(int fd, short events,
                                  zco::Deadline deadline) {
    if (fd < 0)
        return std::make_error_code(std::errc::bad_file_descriptor);
    for (;;) {
        const auto remaining = deadline.time() - zco::Deadline::Clock::now();
        if (remaining <= remaining.zero())
            return std::make_error_code(std::errc::timed_out);
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                .count() +
            1;
        pollfd ready{fd, events, 0};
        const int result =
            ::poll(&ready, 1,
                   static_cast<int>(std::min<int64_t>(milliseconds, INT_MAX)));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return {errno, std::generic_category()};
        if (result == 0)
            continue;
        if (ready.revents & POLLNVAL)
            return std::make_error_code(std::errc::bad_file_descriptor);
        return {};
    }
}

inline znet::Result<znet::Socket> connect_to(const znet::Endpoint &endpoint,
                                             zco::Deadline deadline) {
    auto client =
        znet::Socket::create(endpoint.family(), znet::SocketKind::stream);
    if (!client)
        return client.error();
    const int fd = client.value().native_handle();
    if (::connect(fd, endpoint.native_address(), endpoint.native_size()) != 0) {
        if (errno != EINPROGRESS && errno != EINTR)
            return znet::io_error("test client connect", errno);
        if (auto error = wait_ready(fd, POLLOUT, deadline))
            return znet::runtime_error("test client connect", error);
        auto error = client.value().option<int>(SOL_SOCKET, SO_ERROR);
        if (!error)
            return error.error();
        if (error.value())
            return znet::io_error("test client connect", error.value());
    }
    return std::move(client).value();
}

inline znet::Result<znet::Socket> connect_to(const HttpServer &server) {
    auto endpoint = server.local_endpoint();
    if (!endpoint)
        return endpoint.error();
    return connect_to(endpoint.value(),
                      zco::Deadline::after(std::chrono::seconds(1)));
}

inline bool send_all(int fd, std::string_view data, int timeout_ms = 1000) {
    const auto deadline =
        zco::Deadline::after(std::chrono::milliseconds(timeout_ms));
    size_t sent = 0;
    while (sent < data.size()) {
        if (deadline.expired(zco::Deadline::Clock::now()))
            return false;
        const auto count = ::send(fd, data.data() + sent, data.size() - sent,
                                  MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_ready(fd, POLLOUT, deadline))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

/**
 * @brief 保存一次读取中的多余字节，后续头部或帧继续从同一接收缓冲中消费。
 */
class SocketReader {
  public:
    explicit SocketReader(int fd, size_t read_limit = 4096)
        : fd_(fd), read_limit_(std::max<size_t>(1, read_limit)) {}

    zco::Result<std::string> exact(size_t size, zco::Deadline deadline) {
        while (buffer_.size() < size) {
            auto received = read_more(deadline);
            if (!received)
                return zco::Result<std::string>(received.error());
            if (!received.value())
                return zco::Result<std::string>(
                    std::make_error_code(std::errc::connection_reset));
        }
        return consume(size);
    }

    zco::Result<std::string> until(std::string_view terminator,
                                   zco::Deadline deadline) {
        for (;;) {
            const auto end = buffer_.find(terminator);
            if (end != std::string::npos)
                return consume(end + terminator.size());
            auto received = read_more(deadline);
            if (!received)
                return zco::Result<std::string>(received.error());
            if (!received.value())
                return zco::Result<std::string>(
                    std::make_error_code(std::errc::connection_reset));
        }
    }

    zco::Result<std::string> until_closed(zco::Deadline deadline) {
        for (;;) {
            auto received = read_more(deadline);
            if (!received)
                return zco::Result<std::string>(received.error());
            if (!received.value())
                return consume(buffer_.size());
        }
    }

  private:
    zco::Result<std::string> consume(size_t size) {
        std::string result = buffer_.substr(0, size);
        buffer_.erase(0, size);
        return zco::Result<std::string>(std::move(result));
    }

    zco::Result<bool> read_more(zco::Deadline deadline) {
        for (;;) {
            if (auto error = wait_ready(fd_, POLLIN, deadline))
                return zco::Result<bool>(error);
            char bytes[4096];
            const auto count = ::recv(
                fd_, bytes, std::min(sizeof(bytes), read_limit_), MSG_DONTWAIT);
            if (count > 0) {
                buffer_.append(bytes, static_cast<size_t>(count));
                return zco::Result<bool>(true);
            }
            if (count == 0)
                return zco::Result<bool>(false);
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                return zco::Result<bool>(
                    std::error_code(errno, std::generic_category()));
        }
    }

    int fd_;
    size_t read_limit_;
    std::string buffer_;
};

inline std::string recv_until_close(int fd, int timeout_ms) {
    return SocketReader(fd)
        .until_closed(
            zco::Deadline::after(std::chrono::milliseconds(timeout_ms)))
        .value();
}

inline zco::Result<std::string> recv_websocket_frame(SocketReader &reader,
                                                     zco::Deadline deadline) {
    auto header = reader.exact(2, deadline);
    if (!header)
        return header;
    std::string frame = std::move(header).value();
    uint64_t size = static_cast<uint8_t>(frame[1]) & 0x7f;
    if (size == 126 || size == 127) {
        auto length = reader.exact(size == 126 ? 2 : 8, deadline);
        if (!length)
            return length;
        size = 0;
        for (unsigned char byte : length.value())
            size = (size << 8) | byte;
        frame += length.value();
    }
    if (size > kDefaultWebSocketMaxMessageSize)
        return zco::Result<std::string>(
            std::make_error_code(std::errc::message_size));
    const size_t mask_size = (static_cast<uint8_t>(frame[1]) & 0x80) ? 4 : 0;
    auto payload =
        reader.exact(mask_size + static_cast<size_t>(size), deadline);
    if (!payload)
        return payload;
    frame += payload.value();
    return zco::Result<std::string>(std::move(frame));
}

} // namespace zhttp::test

#endif
