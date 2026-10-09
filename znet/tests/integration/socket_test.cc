#include "../support/network_fixture.h"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>

using namespace znet;
using namespace std::chrono_literals;

TEST(SocketTest, CreationAdoptionAndMoveKeepUniqueNonblockingOwnership) {
    auto socket = Socket::create(AF_INET, SocketKind::stream);
    ASSERT_TRUE(socket);
    const int fd = socket.value().native_handle();
    EXPECT_NE(::fcntl(fd, F_GETFL) & O_NONBLOCK, 0);
    EXPECT_NE(::fcntl(fd, F_GETFD) & FD_CLOEXEC, 0);
    Socket moved = std::move(socket).value();
    EXPECT_EQ(socket.value().native_handle(), -1);
    EXPECT_EQ(moved.native_handle(), fd);
    EXPECT_TRUE(moved.close());
    EXPECT_TRUE(moved.close());
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_FALSE(Socket::create(AF_PACKET, SocketKind::stream));
    EXPECT_FALSE(Socket::adopt(-1));
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    EXPECT_FALSE(Socket::adopt(pipefd[0]));
    EXPECT_EQ(::fcntl(pipefd[0], F_GETFD), -1);
    ::close(pipefd[1]);
}

TEST(SocketTest, StreamConnectAcceptAndEndpointQueriesWorkForBothIpFamilies) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    for (int family : {AF_INET, AF_INET6}) {
        auto listener = Socket::create(family, SocketKind::stream);
        ASSERT_TRUE(listener);
        auto endpoint = family == AF_INET ? Endpoint::ipv4("127.0.0.1", 0)
                                          : Endpoint::ipv6("::1", 0);
        ASSERT_TRUE(listener.value().bind(endpoint.value()));
        ASSERT_TRUE(listener.value().listen(8));
        auto destination = listener.value().local_endpoint();
        ASSERT_TRUE(destination);
        EXPECT_GT(destination.value().port(), 0);
        auto client = Socket::create(family, SocketKind::stream);
        ASSERT_TRUE(client);
        auto accept = runtime.spawn([&] {
            auto accepted = listener.value().accept(zco::Deadline::after(1s));
            ASSERT_TRUE(accepted);
            char bytes[4]{};
            auto received =
                accepted.value().read_some(bytes, 4, zco::Deadline::after(1s));
            ASSERT_TRUE(received);
            EXPECT_EQ(received.bytes, 4u);
            EXPECT_EQ(std::string(bytes, 4), "ping");
            EXPECT_TRUE(accepted.value().remote_endpoint());
        });
        auto connect = runtime.spawn([&] {
            ASSERT_TRUE(client.value().connect(destination.value(),
                                               zco::Deadline::after(1s)));
            auto sent =
                client.value().write_some("ping", 4, zco::Deadline::after(1s));
            EXPECT_TRUE(sent);
            EXPECT_EQ(sent.bytes, 4u);
            EXPECT_TRUE(client.value().local_endpoint());
            EXPECT_EQ(client.value().remote_endpoint().value().port(),
                      destination.value().port());
        });
        ASSERT_TRUE(accept);
        ASSERT_TRUE(connect);
        EXPECT_TRUE(accept.value().join());
        EXPECT_TRUE(connect.value().join());
    }
}

TEST(SocketTest, UdpPreservesSourceEmptyDatagramsAndTruncation) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto receiver = Socket::create(AF_INET, SocketKind::datagram);
    auto sender = Socket::create(AF_INET, SocketKind::datagram);
    ASSERT_TRUE(receiver);
    ASSERT_TRUE(sender);
    ASSERT_TRUE(receiver.value().bind(Endpoint::ipv4("127.0.0.1", 0).value()));
    auto destination = receiver.value().local_endpoint().value();
    auto task = runtime.spawn([&] {
        auto empty =
            sender.value().send_to({}, destination, zco::Deadline::after(1s));
        ASSERT_TRUE(empty);
        EXPECT_EQ(empty.bytes, 0u);
        char bytes[2];
        auto first = receiver.value().receive_from(bytes, sizeof(bytes),
                                                   zco::Deadline::after(1s));
        ASSERT_TRUE(first);
        EXPECT_EQ(first.value().bytes, 0u);
        EXPECT_FALSE(first.value().truncated);
        EXPECT_GT(first.value().source.port(), 0);
        EXPECT_TRUE(sender.value().send_to("abcd", destination,
                                           zco::Deadline::after(1s)));
        auto second = receiver.value().receive_from(bytes, sizeof(bytes),
                                                    zco::Deadline::after(1s));
        ASSERT_TRUE(second);
        EXPECT_EQ(second.value().bytes, 2u);
        EXPECT_TRUE(second.value().truncated);
        EXPECT_EQ(std::string(bytes, 2), "ab");
    });
    ASSERT_TRUE(task);
    EXPECT_TRUE(task.value().join());
    EXPECT_FALSE(receiver.value().listen());
    EXPECT_FALSE(receiver.value().read_some(nullptr, 1));
}

TEST(SocketTest, UnixAbstractStreamAndDatagramAddressesRoundTrip) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    for (auto kind : {SocketKind::stream, SocketKind::datagram}) {
        const auto name = std::string("\0znet-test-", 11) +
                          std::to_string(::getpid()) +
                          (kind == SocketKind::stream ? "s" : "d");
        auto endpoint = Endpoint::unix_domain(name).value();
        auto listener = Socket::create(AF_UNIX, kind);
        auto client = Socket::create(AF_UNIX, kind);
        ASSERT_TRUE(listener);
        ASSERT_TRUE(client);
        ASSERT_TRUE(listener.value().bind(endpoint));
        EXPECT_EQ(listener.value().local_endpoint().value().to_string(),
                  endpoint.to_string());
        if (kind == SocketKind::stream)
            ASSERT_TRUE(listener.value().listen());
        else
            ASSERT_TRUE(client.value().bind(
                Endpoint::unix_domain(name + "client").value()));
        auto task = runtime.spawn([&] {
            char bytes[4];
            if (kind == SocketKind::stream) {
                ASSERT_TRUE(
                    client.value().connect(endpoint, zco::Deadline::after(1s)));
                auto accepted =
                    listener.value().accept(zco::Deadline::after(1s));
                ASSERT_TRUE(accepted);
                EXPECT_TRUE(client.value().write_some(
                    "unix", 4, zco::Deadline::after(1s)));
                EXPECT_EQ(accepted.value()
                              .read_some(bytes, 4, zco::Deadline::after(1s))
                              .bytes,
                          4u);
            } else {
                EXPECT_TRUE(client.value().send_to("unix", endpoint,
                                                   zco::Deadline::after(1s)));
                auto received = listener.value().receive_from(
                    bytes, 4, zco::Deadline::after(1s));
                ASSERT_TRUE(received);
                EXPECT_EQ(received.value().source.to_string(),
                          "@" + name.substr(1) + "client");
            }
            EXPECT_EQ(std::string(bytes, 4), "unix");
        });
        ASSERT_TRUE(task);
        EXPECT_TRUE(task.value().join());
    }
}

TEST(SocketTest, UnixDatagramsFromUnnamedSendersKeepTheirPayload) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    const auto name =
        std::string("\0znet-unnamed-", 14) + std::to_string(::getpid());
    auto endpoint = Endpoint::unix_domain(name).value();
    auto receiver = Socket::create(AF_UNIX, SocketKind::datagram);
    auto sender = Socket::create(AF_UNIX, SocketKind::datagram);
    ASSERT_TRUE(receiver);
    ASSERT_TRUE(sender);
    ASSERT_TRUE(receiver.value().bind(endpoint));
    auto task = runtime.spawn([&] {
        for (auto payload : {std::string_view{}, std::string_view{"unix"}}) {
            ASSERT_TRUE(sender.value().send_to(payload, endpoint,
                                               zco::Deadline::after(1s)));
            char bytes[4];
            auto received = receiver.value().receive_from(
                bytes, sizeof(bytes), zco::Deadline::after(1s));
            ASSERT_TRUE(received);
            EXPECT_EQ(received.value().bytes, payload.size());
            EXPECT_EQ(received.value().source.family(), AF_UNIX);
            EXPECT_TRUE(received.value().source.to_string().empty());
            EXPECT_FALSE(received.value().truncated);
            EXPECT_EQ(std::string_view(bytes, received.value().bytes), payload);
        }
    });
    ASSERT_TRUE(task);
    EXPECT_TRUE(task.value().join());
}

TEST(SocketTest, TimeoutContextAndClosedDescriptorErrorsAreStructured) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto listener = Socket::create(AF_INET, SocketKind::stream);
    ASSERT_TRUE(listener);
    ASSERT_TRUE(listener.value().bind(Endpoint::ipv4("127.0.0.1", 0).value()));
    ASSERT_TRUE(listener.value().listen());
    auto outside = listener.value().accept();
    ASSERT_FALSE(outside);
    EXPECT_EQ(outside.error().code, std::errc::operation_not_permitted);
    auto task = runtime.spawn([&] {
        auto timed = listener.value().accept(zco::Deadline::after(20ms));
        ASSERT_FALSE(timed);
        EXPECT_EQ(timed.error().code, std::errc::timed_out);
        EXPECT_TRUE(listener.value().close());
        auto closed = listener.value().accept();
        ASSERT_FALSE(closed);
        EXPECT_EQ(closed.error().code, std::errc::bad_file_descriptor);
    });
    ASSERT_TRUE(task);
    EXPECT_TRUE(task.value().join());
    EXPECT_FALSE(listener.value().local_endpoint());
    EXPECT_FALSE(listener.value().option<int>(SOL_SOCKET, SO_ERROR));
}
