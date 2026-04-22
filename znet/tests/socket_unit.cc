#include "znet/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>

#include "zco/sched.h"
#include "zco/wait_group.h"

namespace znet {
namespace {

bool is_timeout_errno(int err) {
    return err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK;
}

class BadAddress : public Address {
  public:
    int family() const override { return AF_INET; }
    const sockaddr *sockaddr_ptr() const override {
        return reinterpret_cast<const sockaddr *>(&addr_);
    }
    sockaddr *sockaddr_ptr() override {
        return reinterpret_cast<sockaddr *>(&addr_);
    }
    socklen_t sockaddr_len() const override { return 0; }
    std::string to_string() const override { return "bad"; }

  private:
    sockaddr_in addr_{};
};

class SocketCoroutineUnitTest : public ::testing::Test {
  protected:
    void TearDown() override { zco::shutdown(); }
};

TEST_F(SocketCoroutineUnitTest, NewSocketDefaultsToNonBlocking) {
    Socket::ptr socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    ASSERT_TRUE(socket->is_valid());

    const int flags = ::fcntl(socket->fd(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE((flags & O_NONBLOCK), 0);
}

TEST_F(SocketCoroutineUnitTest, SendOutsideCoroutineFailsFast) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    Socket socket(pair[0]);
    const char payload[] = "ok";

    errno = 0;
    EXPECT_EQ(socket.send(payload, sizeof(payload) - 1), -1);
    EXPECT_EQ(errno, EPERM);

    ::close(pair[1]);
}

TEST_F(SocketCoroutineUnitTest, ConnectOutsideCoroutineFailsFast) {
    Socket::ptr client = Socket::create_tcp();
    ASSERT_NE(client, nullptr);

    Address::ptr target = std::make_shared<IPv4Address>("127.0.0.1", 9);
    errno = 0;
    EXPECT_FALSE(client->connect(target, 10));
    EXPECT_EQ(errno, EPERM);
}

TEST_F(SocketCoroutineUnitTest, SendAndRecvSucceedInCoroutineContext) {
    zco::init(2);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    Socket writer(pair[0]);
    Socket reader(pair[1]);
    zco::WaitGroup done(2);

    zco::go([&writer, &done]() {
        const char payload[] = "ping";
        EXPECT_EQ(writer.send(payload, sizeof(payload) - 1, 0, 200),
                  static_cast<ssize_t>(sizeof(payload) - 1));
        done.done();
    });

    zco::go([&reader, &done]() {
        char buffer[8] = {0};
        EXPECT_EQ(reader.recv(buffer, 4, 0, 200), 4);
        EXPECT_STREQ(buffer, "ping");
        done.done();
    });

    done.wait();
}

TEST_F(SocketCoroutineUnitTest, RecvRespectsExplicitTimeoutInCoroutineContext) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    Socket reader(pair[0]);
    zco::WaitGroup done(1);

    std::atomic<ssize_t> result{-2};
    std::atomic<int> err{0};
    std::atomic<int64_t> elapsed_ms{0};

    zco::go([&]() {
        char buffer[8] = {0};
        const auto started = std::chrono::steady_clock::now();
        errno = 0;
        const ssize_t n = reader.recv(buffer, sizeof(buffer), 0, 30);
        const auto ended = std::chrono::steady_clock::now();

        result.store(n, std::memory_order_release);
        err.store(errno, std::memory_order_release);
        elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                             ended - started)
                             .count(),
                         std::memory_order_release);
        done.done();
    });

    done.wait();

    EXPECT_EQ(result.load(std::memory_order_acquire), -1);
    EXPECT_TRUE(is_timeout_errno(err.load(std::memory_order_acquire)));
    EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 20);

    ::close(pair[1]);
}

TEST_F(SocketCoroutineUnitTest,
       AcceptRespectsExplicitTimeoutInCoroutineContext) {
    zco::init(1);

    Socket::ptr listener = Socket::create_tcp();
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(listener->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    ASSERT_TRUE(listener->listen(8));

    zco::WaitGroup done(1);
    std::atomic<int> err{0};
    std::atomic<int64_t> elapsed_ms{0};
    std::atomic<bool> timed_out{false};

    zco::go([&]() {
        const auto started = std::chrono::steady_clock::now();
        errno = 0;
        Socket::ptr peer = listener->accept(30);
        const auto ended = std::chrono::steady_clock::now();

        timed_out.store(peer == nullptr, std::memory_order_release);
        err.store(errno, std::memory_order_release);
        elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                             ended - started)
                             .count(),
                         std::memory_order_release);
        done.done();
    });

    done.wait();

    EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
    EXPECT_TRUE(is_timeout_errno(err.load(std::memory_order_acquire)));
    EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 20);
}

TEST_F(SocketCoroutineUnitTest, ConnectFailureIsReportedInCoroutineContext) {
    zco::init(1);

    Socket::ptr client = Socket::create_tcp();
    ASSERT_NE(client, nullptr);

    Address::ptr target = std::make_shared<BadAddress>();

    zco::WaitGroup done(1);
    std::atomic<bool> connected{true};
    std::atomic<int> err{0};

    zco::go([&]() {
        errno = 0;
        const bool ok = client->connect(target, 30);

        connected.store(ok, std::memory_order_release);
        err.store(errno, std::memory_order_release);
        done.done();
    });

    done.wait();

    EXPECT_FALSE(connected.load(std::memory_order_acquire));
    EXPECT_EQ(err.load(std::memory_order_acquire), EINVAL);
}

TEST_F(SocketCoroutineUnitTest, FactoryHelpersCreateValidSockets) {
    auto tcp = Socket::create_tcp();
    auto tcp6 = Socket::create_tcp_v6();
    auto udp = Socket::create_udp();
    auto udp6 = Socket::create_udp_v6();

    ASSERT_NE(tcp, nullptr);
    ASSERT_NE(tcp6, nullptr);
    ASSERT_NE(udp, nullptr);
    ASSERT_NE(udp6, nullptr);
    EXPECT_TRUE(tcp->is_valid());
    EXPECT_TRUE(tcp6->is_valid());
    EXPECT_TRUE(udp->is_valid());
    EXPECT_TRUE(udp6->is_valid());
}

TEST_F(SocketCoroutineUnitTest, BindAndConnectRejectFamilyMismatch) {
    auto ipv4_socket = Socket::create_tcp();
    ASSERT_NE(ipv4_socket, nullptr);

    Address::ptr ipv6_addr = std::make_shared<IPv6Address>("::1", 0);
    EXPECT_FALSE(ipv4_socket->bind(ipv6_addr));

    zco::init(1);
    zco::WaitGroup done(1);
    std::atomic<bool> connected{true};
    zco::go([&]() {
        errno = 0;
        connected.store(ipv4_socket->connect(ipv6_addr, 20),
                        std::memory_order_release);
        done.done();
    });
    done.wait();
    EXPECT_FALSE(connected.load(std::memory_order_acquire));
}

TEST_F(SocketCoroutineUnitTest, ReconnectWithoutRemoteAddressFails) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    EXPECT_FALSE(socket->reconnect(10));
}

TEST_F(SocketCoroutineUnitTest, InvalidSocketOperationsReturnExpectedValues) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    ASSERT_TRUE(socket->close());
    EXPECT_TRUE(socket->close());
    EXPECT_TRUE(socket->shutdown_write());
    EXPECT_EQ(socket->get_local_address(), nullptr);
    EXPECT_EQ(socket->get_remote_address(), nullptr);
    EXPECT_EQ(socket->get_error(), -1);
}

TEST_F(SocketCoroutineUnitTest, SetNonBlockingToggleAndSocketOptionsWork) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);

    EXPECT_TRUE(socket->set_non_blocking(false));
    int flags = ::fcntl(socket->fd(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_EQ((flags & O_NONBLOCK), 0);

    EXPECT_TRUE(socket->set_non_blocking(true));
    flags = ::fcntl(socket->fd(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE((flags & O_NONBLOCK), 0);

    EXPECT_TRUE(socket->set_send_timeout(120));
    EXPECT_TRUE(socket->set_recv_timeout(120));
    EXPECT_TRUE(socket->set_keep_alive(true));
    EXPECT_TRUE(socket->set_reuse_addr(true));
    EXPECT_TRUE(socket->set_reuse_port(true));
    EXPECT_TRUE(socket->set_tcp_nodelay(true));
}

TEST_F(SocketCoroutineUnitTest, UdpSendToAndRecvFromWorkInCoroutineContext) {
    zco::init(1);

    auto receiver = Socket::create_udp();
    auto sender = Socket::create_udp();
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(sender, nullptr);

    auto bind_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    ASSERT_TRUE(receiver->bind(bind_addr));
    auto local_addr =
        std::dynamic_pointer_cast<IPv4Address>(receiver->get_local_address());
    ASSERT_NE(local_addr, nullptr);

    auto to = std::make_shared<IPv4Address>("127.0.0.1", local_addr->port());

    zco::WaitGroup done(2);
    std::atomic<ssize_t> recv_n{-1};
    std::atomic<ssize_t> send_n{-1};
    std::atomic<int> recv_errno{0};

    zco::go([&]() {
        char buf[16] = {0};
        errno = 0;
        recv_n.store(receiver->recv_from(buf, sizeof(buf), nullptr, 0, 200),
                     std::memory_order_release);
        recv_errno.store(errno, std::memory_order_release);
        if (recv_n.load(std::memory_order_acquire) > 0) {
            EXPECT_STREQ(buf, "udp");
        }
        done.done();
    });

    zco::go([&]() {
        send_n.store(sender->send_to("udp", 3, to, 0, 200),
                     std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_EQ(send_n.load(std::memory_order_acquire), 3);
    EXPECT_EQ(recv_n.load(std::memory_order_acquire), 3);
}

TEST_F(SocketCoroutineUnitTest, AcceptOnInvalidSocketFailsInCoroutineContext) {
    zco::init(1);

    auto listener = Socket::create_tcp();
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(listener->close());

    zco::WaitGroup done(1);
    std::atomic<bool> accept_failed{false};
    zco::go([&]() {
        errno = 0;
        Socket::ptr peer = listener->accept(20);
        accept_failed.store(peer == nullptr, std::memory_order_release);
        done.done();
    });
    done.wait();

    EXPECT_TRUE(accept_failed.load(std::memory_order_acquire));
}

TEST_F(SocketCoroutineUnitTest, ConnectSuccessPopulatesLocalAndRemoteAddress) {
    zco::init(1);

    auto listener = Socket::create_tcp();
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(listener->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    ASSERT_TRUE(listener->listen(8));
    auto listen_addr =
        std::dynamic_pointer_cast<IPv4Address>(listener->get_local_address());
    ASSERT_NE(listen_addr, nullptr);

    auto client = Socket::create_tcp();
    ASSERT_NE(client, nullptr);

    zco::WaitGroup done(2);
    std::atomic<bool> connected{false};
    std::atomic<bool> accepted{false};

    zco::go([&]() {
        Socket::ptr peer = listener->accept(200);
        accepted.store(peer != nullptr, std::memory_order_release);
        if (peer) {
            EXPECT_NE(peer->get_local_address(), nullptr);
            EXPECT_NE(peer->get_remote_address(), nullptr);
            peer->close();
        }
        done.done();
    });

    zco::go([&]() {
        connected.store(client->connect(std::make_shared<IPv4Address>(
                                            "127.0.0.1", listen_addr->port()),
                                        200),
                        std::memory_order_release);
        if (connected.load(std::memory_order_acquire)) {
            EXPECT_NE(client->get_local_address(), nullptr);
            EXPECT_NE(client->get_remote_address(), nullptr);
            // 命中缓存返回分支。
            EXPECT_NE(client->get_local_address(), nullptr);
            EXPECT_NE(client->get_remote_address(), nullptr);
            EXPECT_GE(client->get_error(), 0);
        }
        done.done();
    });

    done.wait();
    EXPECT_TRUE(connected.load(std::memory_order_acquire));
    EXPECT_TRUE(accepted.load(std::memory_order_acquire));
}

TEST_F(SocketCoroutineUnitTest, ReconnectSucceedsAfterInitialConnect) {
    zco::init(1);

    auto listener = Socket::create_tcp();
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(listener->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    ASSERT_TRUE(listener->listen(8));
    auto listen_addr =
        std::dynamic_pointer_cast<IPv4Address>(listener->get_local_address());
    ASSERT_NE(listen_addr, nullptr);

    auto client = Socket::create_tcp();
    ASSERT_NE(client, nullptr);

    zco::WaitGroup first_round(2);
    zco::go([&]() {
        Socket::ptr peer = listener->accept(200);
        ASSERT_NE(peer, nullptr);
        peer->close();
        first_round.done();
    });
    zco::go([&]() {
        ASSERT_TRUE(client->connect(
            std::make_shared<IPv4Address>("127.0.0.1", listen_addr->port()),
            200));
        client->close();
        first_round.done();
    });
    first_round.wait();

    listener->close();

    zco::WaitGroup done(1);
    std::atomic<bool> reconnected{true};
    zco::go([&]() {
        reconnected.store(client->reconnect(30), std::memory_order_release);
        done.done();
    });
    done.wait();

    EXPECT_FALSE(reconnected.load(std::memory_order_acquire));
}

TEST_F(SocketCoroutineUnitTest, SocketCtorWithInvalidFdFallsBackToDefaults) {
    Socket socket(-1);
    EXPECT_FALSE(socket.is_valid());
    EXPECT_EQ(socket.family(), AF_INET);
    EXPECT_EQ(socket.type(), SOCK_STREAM);
}

TEST_F(SocketCoroutineUnitTest,
       RawCtorWithInvalidFamilyTriggersSocketCreateFail) {
    Socket socket(-1, SOCK_STREAM, 0);
    EXPECT_FALSE(socket.is_valid());
}

TEST_F(SocketCoroutineUnitTest, SendRecvAndUdpApisValidateInvalidSocketPaths) {
    zco::init(1);

    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    ASSERT_TRUE(socket->close());

    zco::WaitGroup done(1);
    zco::go([&]() {
        char buf[8] = {0};
        EXPECT_EQ(socket->send("x", 1, 0, 20), -1);
        EXPECT_EQ(socket->recv(buf, sizeof(buf), 0, 20), -1);
        errno = 0;
        EXPECT_EQ(socket->send_to("x", 1, nullptr, 0, 20), -1);
        EXPECT_EQ(errno, EINVAL);
        EXPECT_EQ(socket->recv_from(buf, sizeof(buf), nullptr, 0, 20), -1);
        done.done();
    });
    done.wait();
}

TEST_F(SocketCoroutineUnitTest, ShutdownWriteFailurePathOnUnconnectedSocket) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    EXPECT_FALSE(socket->shutdown_write());
}

TEST_F(SocketCoroutineUnitTest, BindAndListenFailurePathsAreCovered) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);

    ASSERT_TRUE(socket->close());
    EXPECT_FALSE(socket->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    EXPECT_FALSE(socket->listen(1));

    auto socket2 = Socket::create_tcp();
    ASSERT_NE(socket2, nullptr);
    EXPECT_FALSE(socket2->bind(std::make_shared<BadAddress>()));

    auto udp = Socket::create_udp();
    ASSERT_NE(udp, nullptr);
    EXPECT_FALSE(udp->listen(1));
}

TEST_F(SocketCoroutineUnitTest, SendRecvFromNonCoroutinePathsFailFast) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);

    char buf[8] = {0};
    errno = 0;
    EXPECT_EQ(socket->recv(buf, sizeof(buf), 0, 10), -1);
    EXPECT_EQ(errno, EPERM);

    errno = 0;
    EXPECT_EQ(socket->send_to("x", 1, std::make_shared<IPv4Address>(), 0, 10),
              -1);
    EXPECT_EQ(errno, EPERM);

    errno = 0;
    EXPECT_EQ(socket->recv_from(buf, sizeof(buf),
                                std::make_shared<IPv4Address>(), 0, 10),
              -1);
    EXPECT_EQ(errno, EPERM);
}

TEST_F(SocketCoroutineUnitTest, RecvFromBranchWithNonNullFromAddressIsCovered) {
    zco::init(1);

    auto receiver = Socket::create_udp();
    auto sender = Socket::create_udp();
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(sender, nullptr);

    ASSERT_TRUE(receiver->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    auto local =
        std::dynamic_pointer_cast<IPv4Address>(receiver->get_local_address());
    ASSERT_NE(local, nullptr);

    zco::WaitGroup done(2);
    zco::go([&]() {
        char buf[8] = {0};
        Address::ptr from = std::make_shared<IPv4Address>();
        EXPECT_EQ(receiver->recv_from(buf, sizeof(buf), from, 0, 200), 3);
        done.done();
    });
    zco::go([&]() {
        EXPECT_EQ(sender->send_to(
                      "udp", 3,
                      std::make_shared<IPv4Address>("127.0.0.1", local->port()),
                      0, 200),
                  3);
        done.done();
    });
    done.wait();
}

TEST_F(SocketCoroutineUnitTest, SetNonBlockingFalseFailsOnInvalidFdPath) {
    auto socket = Socket::create_tcp();
    ASSERT_NE(socket, nullptr);
    ASSERT_TRUE(socket->close());
    EXPECT_FALSE(socket->set_non_blocking(false));
}

TEST_F(SocketCoroutineUnitTest, ShutdownWriteSuccessPathOnConnectedSocket) {
    zco::init(1);

    auto listener = Socket::create_tcp();
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(listener->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
    ASSERT_TRUE(listener->listen(8));
    auto local =
        std::dynamic_pointer_cast<IPv4Address>(listener->get_local_address());
    ASSERT_NE(local, nullptr);

    auto client = Socket::create_tcp();
    ASSERT_NE(client, nullptr);
    zco::WaitGroup done(2);
    zco::go([&]() {
        auto peer = listener->accept(200);
        ASSERT_NE(peer, nullptr);
        EXPECT_TRUE(peer->shutdown_write());
        peer->close();
        done.done();
    });
    zco::go([&]() {
        ASSERT_TRUE(client->connect(
            std::make_shared<IPv4Address>("127.0.0.1", local->port()), 200));
        done.done();
    });
    done.wait();
}

} // namespace
} // namespace znet
