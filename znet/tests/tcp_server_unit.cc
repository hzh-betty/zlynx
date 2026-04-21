#define private public
#include "znet/tcp_server.h"
#undef private

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "znet/tls_context.h"

namespace znet {
namespace {

class AlwaysFailTlsContext : public TlsContext {
  public:
    std::unique_ptr<TlsChannel> create_server_channel(int) const override {
        errno = EIO;
        return nullptr;
    }
};

std::pair<std::string, std::string> create_temp_cert_pair() {
    char dir_template[] = "/tmp/znet-server-tls-XXXXXX";
    char *dir = ::mkdtemp(dir_template);
    EXPECT_NE(dir, nullptr);
    const std::string cert = std::string(dir ? dir : "/tmp") + "/cert.pem";
    const std::string key = std::string(dir ? dir : "/tmp") + "/key.pem";
    const std::string cmd =
        "openssl req -x509 -nodes -newkey rsa:2048 -days 1 "
        "-subj '/CN=localhost' -keyout " +
        key + " -out " + cert + " >/dev/null 2>&1";
    EXPECT_EQ(std::system(cmd.c_str()), 0);
    return {cert, key};
}

class TcpServerUnitTest : public ::testing::Test {
  protected:
    void TearDown() override { zco::shutdown(); }
};

TEST_F(TcpServerUnitTest, EnableTlsRejectsInvalidCertificatePaths) {
    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    EXPECT_FALSE(server->enable_tls("/tmp/not-exist-cert.pem",
                                    "/tmp/not-exist-key.pem"));
}

TEST_F(TcpServerUnitTest, AcceptsConnectionAndConsumesBufferMessages) {
    zco::init(3);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> message_events{0};
    std::atomic<int> total_bytes{0};
    std::mutex payload_mu;
    std::string payload;

    server->set_on_message([&](const TcpConnection::ptr &conn, Buffer &buffer) {
        ASSERT_NE(conn, nullptr);
        const size_t bytes = buffer.readable_bytes();
        total_bytes.fetch_add(static_cast<int>(bytes),
                              std::memory_order_relaxed);
        std::string chunk = buffer.retrieve_all_as_string();
        {
            std::lock_guard<std::mutex> lock(payload_mu);
            payload.append(chunk);
        }
        message_events.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(server->start());
    ASSERT_NE(server->acceptor(), nullptr);
    ASSERT_NE(server->acceptor()->listen_socket(), nullptr);

    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);
    ASSERT_EQ(::send(client_fd, "ping", 4, 0), 4);
    ::close(client_fd);

    for (int i = 0;
         i < 100 && message_events.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(message_events.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(total_bytes.load(std::memory_order_relaxed), 4);
    EXPECT_EQ(payload, "ping");

    server->stop();
}

TEST_F(TcpServerUnitTest, OnMessageCallbackUsesConnectionAndBuffer) {
    zco::init(3);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> callback_count{0};
    std::atomic<int> total_bytes{0};
    std::atomic<int> seen_fd{-1};

    server->set_on_message([&](const TcpConnection::ptr &conn, Buffer &buffer) {
        ASSERT_NE(conn, nullptr);
        seen_fd.store(conn->fd(), std::memory_order_relaxed);
        total_bytes.fetch_add(static_cast<int>(buffer.readable_bytes()),
                              std::memory_order_relaxed);
        (void)buffer.retrieve_all_as_string();
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    auto send_payload = [&](const char *msg) {
        int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client_fd, 0);

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(bound_addr->port());
        ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
        ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr *>(&addr),
                            sizeof(addr)),
                  0);
        ASSERT_EQ(::send(client_fd, msg, 4, 0), 4);
        ASSERT_EQ(::send(client_fd, msg, 4, 0), 4);
        ::close(client_fd);
    };

    send_payload("msg1");

    for (int i = 0;
         i < 100 && callback_count.load(std::memory_order_relaxed) < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(callback_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(total_bytes.load(std::memory_order_relaxed), 8);
    EXPECT_GE(seen_fd.load(std::memory_order_relaxed), 0);

    server->stop();
}

TEST_F(TcpServerUnitTest, ConnectAndCloseCallbacksAreIndependent) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> connect_count{0};
    std::atomic<int> close_count{0};

    server->set_on_connection([&](TcpConnection::ptr conn) {
        ASSERT_NE(conn, nullptr);
        connect_count.fetch_add(1);
    });
    server->set_on_close([&](TcpConnection::ptr conn) {
        ASSERT_NE(conn, nullptr);
        close_count.fetch_add(1);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);
    ::close(client_fd);

    for (int i = 0; i < 100 && connect_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 100 && close_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(connect_count.load(), 1);
    EXPECT_EQ(close_count.load(), 1);

    server->stop();
}

TEST_F(TcpServerUnitTest, SnakeCaseMessageCallbackReceivesBuffer) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> connection_events{0};
    std::atomic<int> message_events{0};
    std::string merged_payload;

    server->set_on_connection([&](const std::shared_ptr<TcpConnection> &conn) {
        EXPECT_NE(conn, nullptr);
        connection_events.fetch_add(1);
    });

    server->set_on_message([&](const TcpConnection::ptr &conn, Buffer &buffer) {
        EXPECT_NE(conn, nullptr);
        merged_payload.append(buffer.retrieve_all_as_string());
        message_events.fetch_add(1);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);
    ASSERT_EQ(::send(client_fd, "abc", 3, 0), 3);
    ASSERT_EQ(::send(client_fd, "def", 3, 0), 3);
    ::close(client_fd);

    for (int i = 0; i < 100 && message_events.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(connection_events.load(), 1);
    EXPECT_GE(message_events.load(), 1);
    EXPECT_EQ(merged_payload, "abcdef");

    server->stop();
}

TEST_F(TcpServerUnitTest, KeepsConnectionOpenUntilPeerCloses) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> total_bytes{0};
    std::atomic<int> close_count{0};

    server->set_on_message([&](const TcpConnection::ptr &conn, Buffer &buffer) {
        ASSERT_NE(conn, nullptr);
        total_bytes.fetch_add(static_cast<int>(buffer.readable_bytes()));
        (void)buffer.retrieve_all_as_string();
    });

    server->set_on_close([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        close_count.fetch_add(1);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);

    ASSERT_EQ(::send(client_fd, "a", 1, 0), 1);
    for (int i = 0; i < 100 && total_bytes.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(total_bytes.load(), 1);
    EXPECT_EQ(close_count.load(), 0);

    ASSERT_EQ(::send(client_fd, "b", 1, 0), 1);
    for (int i = 0; i < 100 && total_bytes.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(total_bytes.load(), 2);
    EXPECT_EQ(close_count.load(), 0);

    ::close(client_fd);

    for (int i = 0; i < 100 && close_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(close_count.load(), 1);

    server->stop();
}

TEST_F(TcpServerUnitTest, StopReturnsPromptlyWhenConnectionIsIdle) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> connect_count{0};
    server->set_on_connection([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        connect_count.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);

    for (int i = 0;
         i < 100 && connect_count.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(connect_count.load(std::memory_order_relaxed), 1);

    // 给连接协程一点时间进入空闲读等待。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    std::atomic<bool> stop_done{false};
    std::thread stopper([&]() {
        server->stop();
        stop_done.store(true, std::memory_order_release);
    });

    bool finished_in_time = false;
    for (int i = 0; i < 50; ++i) {
        if (stop_done.load(std::memory_order_acquire)) {
            finished_in_time = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!finished_in_time) {
        ::close(client_fd);
        client_fd = -1;
    }

    if (stopper.joinable()) {
        stopper.join();
    }

    if (client_fd >= 0) {
        ::close(client_fd);
    }

    EXPECT_TRUE(finished_in_time);
}

TEST_F(TcpServerUnitTest, WriteTimeoutIsAppliedToAcceptedConnection) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    server->set_write_timeout(321);

    std::atomic<uint32_t> observed_write_timeout{0};
    std::atomic<int> connect_count{0};
    server->set_on_connection([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        observed_write_timeout.store(conn->write_timeout(),
                                     std::memory_order_relaxed);
        connect_count.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);

    for (int i = 0;
         i < 100 && connect_count.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(connect_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(observed_write_timeout.load(std::memory_order_relaxed), 321U);

    ::close(client_fd);
    server->stop();
}

TEST_F(TcpServerUnitTest, DoStartFailsWhenAcceptorIsNull) {
    auto server = std::make_shared<TcpServer>(Address::ptr{}, 16);
    ASSERT_NE(server, nullptr);

    server->acceptor_.reset();
    EXPECT_FALSE(server->do_start());
}

TEST_F(TcpServerUnitTest, RegisterAndRemoveConnectionHandleNullAndErase) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    server->register_connection(nullptr);
    EXPECT_TRUE(server->connections_.empty());

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);
    const int fd = conn->fd();
    server->register_connection(conn);
    ASSERT_EQ(server->connections_.size(), 1U);

    server->remove_connection(fd);
    EXPECT_TRUE(server->connections_.empty());

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpServerUnitTest, HandleConnectionIgnoresNullClient) {
    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    server->handle_connection(nullptr);
    EXPECT_TRUE(server->connections_.empty());
}

TEST_F(TcpServerUnitTest, DoStopClearsTrackedConnections) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);
    server->connections_[conn->fd()] = conn;

    server->do_stop();
    EXPECT_TRUE(server->connections_.empty());

    ::close(pair[1]);
}

TEST_F(TcpServerUnitTest, DoStopHandlesNullAcceptorAndNullConnectionEntry) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    server->connections_[-1] = nullptr;
    server->connections_[conn->fd()] = conn;
    server->acceptor_.reset();

    server->do_stop();
    EXPECT_TRUE(server->connections_.empty());

    ::close(pair[1]);
}

TEST_F(TcpServerUnitTest, HandleConnectionDirectPathCoversTlsHandshakeFailure) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);
    server->running_.store(true, std::memory_order_release);
    server->tls_context_ = std::make_shared<AlwaysFailTlsContext>();
    server->tls_handshake_timeout_ms_ = 5;

    server->handle_connection(std::make_shared<Socket>(pair[0]));
    EXPECT_TRUE(server->connections_.empty());

    ::close(pair[1]);
}

TEST_F(TcpServerUnitTest, WriteCompleteAndHighWaterCallbacksAreApplied) {
    zco::init(2);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> conn_count{0};
    server->set_on_connection([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        conn_count.fetch_add(1, std::memory_order_relaxed);
    });
    server->set_on_write_complete(
        [](const TcpConnection::ptr &conn) { ASSERT_NE(conn, nullptr); });
    server->set_on_high_water_mark(
        [](const TcpConnection::ptr &conn, size_t bytes) {
            ASSERT_NE(conn, nullptr);
            EXPECT_GE(bytes, 1U);
        },
        1);

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);
    ::close(client_fd);

    for (int i = 0; i < 100 && conn_count.load(std::memory_order_relaxed) < 1;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(conn_count.load(std::memory_order_relaxed), 1);

    server->stop();
}

TEST_F(TcpServerUnitTest, EnableTlsSuccessPathSetsTlsState) {
    if (std::system("openssl version >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "openssl command is required";
    }

    auto cert_pair = create_temp_cert_pair();
    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    EXPECT_TRUE(server->enable_tls(cert_pair.first, cert_pair.second, 321));
    EXPECT_TRUE(server->tls_enabled());
    EXPECT_EQ(server->tls_handshake_timeout_ms_, 321U);
}

TEST_F(TcpServerUnitTest, HandleConnectionRunsInlineWhenSchedulerIsUnavailable) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto server = std::make_shared<TcpServer>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> close_count{0};
    server->set_on_close([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        close_count.fetch_add(1, std::memory_order_relaxed);
    });

    server->handle_connection(std::make_shared<Socket>(pair[0]));
    for (int i = 0; i < 100 && close_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(close_count.load(std::memory_order_relaxed), 1);

    ::close(pair[1]);
}

TEST_F(TcpServerUnitTest, DataIsConsumedEvenWithoutMessageCallback) {
    zco::init(2);

    auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
    auto server = std::make_shared<TcpServer>(listen_addr, 16);
    ASSERT_NE(server, nullptr);

    std::atomic<int> close_count{0};
    server->set_on_close([&](const TcpConnection::ptr &conn) {
        ASSERT_NE(conn, nullptr);
        close_count.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(server->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        server->acceptor()->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
        0);
    ASSERT_EQ(::send(client_fd, "x", 1, 0), 1);
    ::close(client_fd);

    for (int i = 0; i < 100 && close_count.load(std::memory_order_relaxed) == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(close_count.load(std::memory_order_relaxed), 1);

    server->stop();
}

} // namespace
} // namespace znet
