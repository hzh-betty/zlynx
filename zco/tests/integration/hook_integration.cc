#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zco {
namespace {

class HookIntegrationTest : public test::RuntimeTestBase {};

int send_nosignal_flags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool is_pipe_like_errno(int err) {
    return err == EPIPE || err == ESHUTDOWN || err == ECONNRESET;
}

int make_loopback_listener(sockaddr_in *out_addr) {
    if (!out_addr) {
        errno = EINVAL;
        return -1;
    }

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }

    co_set_reuseaddr(listen_fd);

    std::memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out_addr->sin_port = 0;

    if (co_bind(listen_fd, reinterpret_cast<const sockaddr *>(out_addr),
                static_cast<socklen_t>(sizeof(*out_addr))) != 0) {
        const int err = errno;
        (void)co_close(listen_fd);
        errno = err;
        return -1;
    }

    if (co_listen(listen_fd, 16) != 0) {
        const int err = errno;
        (void)co_close(listen_fd);
        errno = err;
        return -1;
    }

    socklen_t len = static_cast<socklen_t>(sizeof(*out_addr));
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr *>(out_addr),
                      &len) != 0) {
        const int err = errno;
        (void)co_close(listen_fd);
        errno = err;
        return -1;
    }

    return listen_fd;
}

int connect_loopback(const sockaddr_in &target, uint32_t timeout_ms) {
    int fd = co_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (co_connect(fd, reinterpret_cast<const sockaddr *>(&target),
                   static_cast<socklen_t>(sizeof(target)), timeout_ms) != 0) {
        const int err = errno;
        (void)co_close(fd);
        errno = err;
        return -1;
    }

    return fd;
}

TEST_F(HookIntegrationTest, SocketPairReadWriteAndDatagramFlow) {
    init(2);

    int stream_pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);
    ASSERT_EQ(::fcntl(stream_pair[0], F_SETFL,
                      ::fcntl(stream_pair[0], F_GETFL, 0) | O_NONBLOCK),
              0);
    ASSERT_EQ(::fcntl(stream_pair[1], F_SETFL,
                      ::fcntl(stream_pair[1], F_GETFL, 0) | O_NONBLOCK),
              0);

    WaitGroup stream_done(1);
    go([&stream_done, stream_pair]() {
        const char *msg = "hello";
        EXPECT_EQ(co_write(stream_pair[0], msg, 5, 200), 5);

        char recv_buffer[8] = {0};
        EXPECT_EQ(co_read(stream_pair[1], recv_buffer, 5, 200), 5);
        EXPECT_STREQ(recv_buffer, "hello");
        stream_done.done();
    });
    stream_done.wait();

    ::close(stream_pair[0]);
    ::close(stream_pair[1]);

    int dgram_pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram_pair), 0);
    ASSERT_EQ(::fcntl(dgram_pair[0], F_SETFL,
                      ::fcntl(dgram_pair[0], F_GETFL, 0) | O_NONBLOCK),
              0);
    ASSERT_EQ(::fcntl(dgram_pair[1], F_SETFL,
                      ::fcntl(dgram_pair[1], F_GETFL, 0) | O_NONBLOCK),
              0);

    WaitGroup dgram_done(1);
    go([&dgram_done, dgram_pair]() {
        const char *datagram = "hook";
        EXPECT_EQ(co_sendto(dgram_pair[0], datagram, 4, 0, nullptr, 0, 200), 4);

        char datagram_out[8] = {0};
        sockaddr_storage recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        EXPECT_EQ(co_recvfrom(dgram_pair[1], datagram_out, 4, 0,
                              reinterpret_cast<sockaddr *>(&recv_addr),
                              &recv_len, 200),
                  4);
        EXPECT_STREQ(datagram_out, "hook");
        dgram_done.done();
    });
    dgram_done.wait();

    ::close(dgram_pair[0]);
    ::close(dgram_pair[1]);
}

TEST_F(HookIntegrationTest, RecvnRoundTripAndPeerCloseSemantic) {
    init(2);

    int stream_pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);
    ASSERT_EQ(::fcntl(stream_pair[0], F_SETFL,
                      ::fcntl(stream_pair[0], F_GETFL, 0) | O_NONBLOCK),
              0);
    ASSERT_EQ(::fcntl(stream_pair[1], F_SETFL,
                      ::fcntl(stream_pair[1], F_GETFL, 0) | O_NONBLOCK),
              0);

    WaitGroup done(2);

    go([&done, fd = stream_pair[0]]() {
        EXPECT_EQ(co_write(fd, "abc", 3, 200), 3);
        yield();
        EXPECT_EQ(co_write(fd, "def", 3, 200), 3);
        EXPECT_EQ(co_close(fd), 0);
        done.done();
    });

    go([&done, fd = stream_pair[1]]() {
        char payload[7] = {0};
        EXPECT_EQ(co_recvn(fd, payload, 6, 0, 300), 6);
        EXPECT_STREQ(payload, "abcdef");

        char eof_probe = 0;
        EXPECT_EQ(co_recvn(fd, &eof_probe, 1, 0, 200), 0);
        done.done();
    });

    done.wait();
    ::close(stream_pair[1]);
}

TEST_F(HookIntegrationTest, UdpSendtoWithExplicitAddressRoundTrip) {
    init(2);

    int recv_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(recv_fd, 0);
    int send_fd = co_socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(send_fd, 0);

    sockaddr_in recv_addr;
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recv_addr.sin_port = 0;

    ASSERT_EQ(::bind(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                     sizeof(recv_addr)),
              0);
    socklen_t recv_addr_len = sizeof(recv_addr);
    ASSERT_EQ(::getsockname(recv_fd, reinterpret_cast<sockaddr *>(&recv_addr),
                            &recv_addr_len),
              0);

    WaitGroup done(2);

    go([&done, fd = send_fd, recv_addr]() {
        const char *payload = "udp-msg";
        EXPECT_EQ(co_sendto(fd, payload, 7, 0,
                            reinterpret_cast<const sockaddr *>(&recv_addr),
                            static_cast<socklen_t>(sizeof(recv_addr)), 500),
                  7);
        done.done();
    });

    go([&done, fd = recv_fd]() {
        char out[16] = {0};
        sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        EXPECT_EQ(co_recvfrom(fd, out, 7, 0,
                              reinterpret_cast<sockaddr *>(&peer), &peer_len,
                              500),
                  7);
        EXPECT_STREQ(out, "udp-msg");
        done.done();
    });

    done.wait();
    ::close(send_fd);
    ::close(recv_fd);
}

TEST_F(HookIntegrationTest, SingleAcceptorLoopHandlesMultipleClients) {
    init(4);

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);

    int enable = 1;
    ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                           sizeof(enable)),
              0);

    sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;

    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 16), 0);

    socklen_t listen_addr_len = sizeof(listen_addr);
    ASSERT_EQ(::getsockname(listen_fd,
                            reinterpret_cast<sockaddr *>(&listen_addr),
                            &listen_addr_len),
              0);

    constexpr int kClientCount = 4;
    WaitGroup accept_done(1);
    WaitGroup client_done(kClientCount);
    std::atomic<int> accepted(0);

    go([listen_fd, &accept_done, &accepted]() {
        for (int i = 0; i < kClientCount; ++i) {
            sockaddr_in peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            const int fd =
                co_accept4(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                           &peer_len, SOCK_CLOEXEC, 2000);
            EXPECT_GE(fd, 0);
            if (fd >= 0) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                ::close(fd);
            }
        }
        accept_done.done();
    });

    for (int i = 0; i < kClientCount; ++i) {
        go([listen_addr, &client_done]() {
            const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(fd, 0);
            EXPECT_EQ(
                co_connect(fd, reinterpret_cast<const sockaddr *>(&listen_addr),
                           sizeof(listen_addr), 2000),
                0);
            ::close(fd);
            client_done.done();
        });
    }

    accept_done.wait();
    client_done.wait();

    EXPECT_EQ(accepted.load(std::memory_order_relaxed), kClientCount);
    ::close(listen_fd);
}

TEST_F(HookIntegrationTest, RandomizedSocketPairRoundTripDeterministicSeed) {
    init(3);

    int stream_pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);
    ASSERT_EQ(::fcntl(stream_pair[0], F_SETFL,
                      ::fcntl(stream_pair[0], F_GETFL, 0) | O_NONBLOCK),
              0);
    ASSERT_EQ(::fcntl(stream_pair[1], F_SETFL,
                      ::fcntl(stream_pair[1], F_GETFL, 0) | O_NONBLOCK),
              0);

    constexpr int kRounds = 64;
    WaitGroup done(2);
    std::atomic<int> verified(0);

    go([&done, &verified, fd = stream_pair[0]]() {
        std::mt19937 rng(20260329u);
        std::uniform_int_distribution<int> len_dist(1, 128);
        std::uniform_int_distribution<int> ch_dist(0, 25);

        for (int i = 0; i < kRounds; ++i) {
            const int len = len_dist(rng);
            std::string payload;
            payload.resize(static_cast<size_t>(len));
            for (int j = 0; j < len; ++j) {
                payload[static_cast<size_t>(j)] =
                    static_cast<char>('a' + ch_dist(rng));
            }

            uint16_t n = static_cast<uint16_t>(payload.size());
            ASSERT_EQ(co_write(fd, &n, sizeof(n), 500),
                      static_cast<ssize_t>(sizeof(n)));
            ASSERT_EQ(co_write(fd, payload.data(), payload.size(), 500),
                      static_cast<ssize_t>(payload.size()));
            verified.fetch_add(1, std::memory_order_relaxed);
        }

        done.done();
    });

    go([&done, &verified, fd = stream_pair[1]]() {
        std::mt19937 rng(20260329u);
        std::uniform_int_distribution<int> len_dist(1, 128);
        std::uniform_int_distribution<int> ch_dist(0, 25);

        for (int i = 0; i < kRounds; ++i) {
            const int expected_len = len_dist(rng);
            std::string expected;
            expected.resize(static_cast<size_t>(expected_len));
            for (int j = 0; j < expected_len; ++j) {
                expected[static_cast<size_t>(j)] =
                    static_cast<char>('a' + ch_dist(rng));
            }

            uint16_t n = 0;
            ASSERT_EQ(co_read(fd, &n, sizeof(n), 500),
                      static_cast<ssize_t>(sizeof(n)));
            ASSERT_EQ(static_cast<int>(n), expected_len);

            std::string actual;
            actual.resize(static_cast<size_t>(n));
            ASSERT_EQ(co_read(fd, &actual[0], actual.size(), 500),
                      static_cast<ssize_t>(actual.size()));
            ASSERT_EQ(actual, expected);
        }

        done.done();
    });

    done.wait();
    EXPECT_EQ(verified.load(std::memory_order_relaxed), kRounds);

    ::close(stream_pair[0]);
    ::close(stream_pair[1]);
}

TEST_F(HookIntegrationTest, RandomizedRefusedConnectDeterministicSeed) {
    init(2);

    std::mt19937 rng(20260329u);

    for (int i = 0; i < 20; ++i) {
        int probe_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(probe_fd, 0);

        sockaddr_in probe_addr;
        probe_addr.sin_family = AF_INET;
        probe_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        probe_addr.sin_port = 0;
        ASSERT_EQ(::bind(probe_fd, reinterpret_cast<sockaddr *>(&probe_addr),
                         sizeof(probe_addr)),
                  0);

        socklen_t probe_len = sizeof(probe_addr);
        ASSERT_EQ(::getsockname(probe_fd,
                                reinterpret_cast<sockaddr *>(&probe_addr),
                                &probe_len),
                  0);
        const uint16_t closed_port = ntohs(probe_addr.sin_port);
        ::close(probe_fd);

        int fd = co_socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);

        sockaddr_in target_addr;
        target_addr.sin_family = AF_INET;
        target_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target_addr.sin_port =
            htons(static_cast<uint16_t>(closed_port + (rng() % 3)));

        WaitGroup done(1);
        std::atomic<int> rc(0);
        std::atomic<int> captured_errno(0);
        go([&done, &rc, &captured_errno, fd, target_addr]() {
            errno = 0;
            rc.store(
                co_connect(fd, reinterpret_cast<const sockaddr *>(&target_addr),
                           sizeof(target_addr), 120),
                std::memory_order_release);
            captured_errno.store(errno, std::memory_order_release);
            done.done();
        });
        done.wait();

        EXPECT_EQ(rc.load(std::memory_order_acquire), -1);
        const int err = captured_errno.load(std::memory_order_acquire);
        EXPECT_TRUE(err == ECONNREFUSED || err == ETIMEDOUT ||
                    err == EHOSTUNREACH || err == ENETUNREACH ||
                    err == EINVAL || err == EADDRNOTAVAIL);

        ::close(fd);
    }
}

TEST_F(HookIntegrationTest, ConnectFailureThenSuccessPathKeepsRuntimeUsable) {
    init(3);

    int probe_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(probe_fd, 0);
    sockaddr_in refused_addr;
    refused_addr.sin_family = AF_INET;
    refused_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    refused_addr.sin_port = 0;
    ASSERT_EQ(::bind(probe_fd, reinterpret_cast<sockaddr *>(&refused_addr),
                     sizeof(refused_addr)),
              0);
    socklen_t refused_len = sizeof(refused_addr);
    ASSERT_EQ(::getsockname(probe_fd,
                            reinterpret_cast<sockaddr *>(&refused_addr),
                            &refused_len),
              0);
    ::close(probe_fd);

    sockaddr_in listen_addr;
    const int listen_fd = make_loopback_listener(&listen_addr);
    ASSERT_GE(listen_fd, 0);

    WaitGroup done(2);

    go([&done, listen_fd]() {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        const int conn_fd =
            co_accept4(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                       &peer_len, SOCK_CLOEXEC, 2000);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd >= 0) {
            char request[3] = {0};
            EXPECT_EQ(co_recv(conn_fd, request, 2, 0, 1000), 2);
            EXPECT_STREQ(request, "ok");
            EXPECT_EQ(co_send(conn_fd, "ok", 2, send_nosignal_flags(), 1000),
                      2);
            EXPECT_EQ(co_close(conn_fd), 0);
        }
        done.done();
    });

    go([&done, refused_addr, listen_addr]() {
        const int failed_fd = co_socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(failed_fd, 0);
        errno = 0;
        EXPECT_EQ(co_connect(failed_fd,
                             reinterpret_cast<const sockaddr *>(&refused_addr),
                             static_cast<socklen_t>(sizeof(refused_addr)), 120),
                  -1);
        EXPECT_TRUE(errno == ECONNREFUSED || errno == ETIMEDOUT ||
                    errno == EHOSTUNREACH || errno == ENETUNREACH ||
                    errno == EINVAL || errno == EADDRNOTAVAIL);
        EXPECT_EQ(co_close(failed_fd), 0);

        const int success_fd = connect_loopback(listen_addr, 1500);
        EXPECT_GE(success_fd, 0);
        if (success_fd >= 0) {
            EXPECT_EQ(co_send(success_fd, "ok", 2, send_nosignal_flags(), 1000),
                      2);
            char reply[3] = {0};
            EXPECT_EQ(co_recv(success_fd, reply, 2, 0, 1000), 2);
            EXPECT_STREQ(reply, "ok");
            EXPECT_EQ(co_close(success_fd), 0);
        }
        done.done();
    });

    done.wait();
    EXPECT_EQ(co_close(listen_fd), 0);
}

TEST_F(HookIntegrationTest,
       MultiRoundRandomizedAcceptConnectDeterministicSeed) {
    init(4);

    int listen_fd = co_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);

    int enable = 1;
    ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                           sizeof(enable)),
              0);

    sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;

    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&listen_addr),
                     sizeof(listen_addr)),
              0);
    ASSERT_EQ(::listen(listen_fd, 32), 0);

    socklen_t listen_addr_len = sizeof(listen_addr);
    ASSERT_EQ(::getsockname(listen_fd,
                            reinterpret_cast<sockaddr *>(&listen_addr),
                            &listen_addr_len),
              0);

    std::mt19937 rng(20260329u);
    std::uniform_int_distribution<int> client_dist(2, 8);

    constexpr int kRounds = 20;
    int total_expected = 0;
    std::atomic<int> total_accepted(0);

    for (int round = 0; round < kRounds; ++round) {
        const int clients = client_dist(rng);
        total_expected += clients;

        WaitGroup accept_done(1);
        WaitGroup client_done(clients);

        go([listen_fd, clients, &accept_done, &total_accepted]() {
            for (int i = 0; i < clients; ++i) {
                sockaddr_in peer_addr;
                socklen_t peer_len = sizeof(peer_addr);
                const int fd = co_accept4(
                    listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                    &peer_len, SOCK_CLOEXEC, 2000);
                EXPECT_GE(fd, 0);
                if (fd >= 0) {
                    total_accepted.fetch_add(1, std::memory_order_relaxed);
                    ::close(fd);
                }
            }
            accept_done.done();
        });

        for (int i = 0; i < clients; ++i) {
            go([listen_addr, &client_done]() {
                const int fd = co_socket(AF_INET, SOCK_STREAM, 0);
                ASSERT_GE(fd, 0);
                EXPECT_EQ(
                    co_connect(fd,
                               reinterpret_cast<const sockaddr *>(&listen_addr),
                               sizeof(listen_addr), 2000),
                    0);
                ::close(fd);
                client_done.done();
            });
        }

        accept_done.wait();
        client_done.wait();
    }

    EXPECT_EQ(total_accepted.load(std::memory_order_relaxed), total_expected);
    ::close(listen_fd);
}

TEST_F(HookIntegrationTest, TcpLoopbackShutdownWriteModeMatrix) {
    init(2);

    sockaddr_in listen_addr;
    const int listen_fd = make_loopback_listener(&listen_addr);
    ASSERT_GE(listen_fd, 0);

    WaitGroup done(2);

    go([&done, listen_fd]() {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        const int conn_fd =
            co_accept(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                      &peer_len, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        const char *server_msg = "srv";
        EXPECT_EQ(co_send(conn_fd, server_msg, 3, send_nosignal_flags(), 1500),
                  3);
        EXPECT_EQ(co_shutdown(conn_fd, 'w'), 0);

        errno = 0;
        EXPECT_EQ(co_send(conn_fd, "x", 1, send_nosignal_flags(), 200), -1);
        EXPECT_TRUE(is_pipe_like_errno(errno));

        char recv_buf[16] = {0};
        EXPECT_EQ(co_recv(conn_fd, recv_buf, 6, 0, 1500), 6);
        EXPECT_STREQ(recv_buf, "client");

        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    go([&done, listen_addr]() {
        const int conn_fd = connect_loopback(listen_addr, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        char recv_buf[8] = {0};
        EXPECT_EQ(co_recv(conn_fd, recv_buf, 3, 0, 1500), 3);
        EXPECT_STREQ(recv_buf, "srv");

        EXPECT_EQ(co_send(conn_fd, "client", 6, send_nosignal_flags(), 1500),
                  6);

        char eof_probe = 0;
        EXPECT_EQ(co_recv(conn_fd, &eof_probe, 1, 0, 1500), 0);

        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    done.wait();
    EXPECT_EQ(co_close(listen_fd), 0);
}

TEST_F(HookIntegrationTest, TcpLoopbackShutdownReadModeMatrix) {
    init(2);

    sockaddr_in listen_addr;
    const int listen_fd = make_loopback_listener(&listen_addr);
    ASSERT_GE(listen_fd, 0);

    WaitGroup done(2);

    go([&done, listen_fd]() {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        const int conn_fd =
            co_accept(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                      &peer_len, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        EXPECT_EQ(co_shutdown(conn_fd, 'r'), 0);
        EXPECT_EQ(co_send(conn_fd, "srv", 3, send_nosignal_flags(), 1500), 3);

        char recv_buf[8] = {0};
        const ssize_t n = co_recv(conn_fd, recv_buf, 3, 0, 300);
        EXPECT_LE(n, 0);

        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    go([&done, listen_addr]() {
        const int conn_fd = connect_loopback(listen_addr, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        char recv_buf[8] = {0};
        EXPECT_EQ(co_recv(conn_fd, recv_buf, 3, 0, 1500), 3);
        EXPECT_STREQ(recv_buf, "srv");

        EXPECT_EQ(co_send(conn_fd, "cli", 3, send_nosignal_flags(), 1500), 3);
        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    done.wait();
    EXPECT_EQ(co_close(listen_fd), 0);
}

TEST_F(HookIntegrationTest, TcpLoopbackShutdownBothModeMatrix) {
    init(2);

    sockaddr_in listen_addr;
    const int listen_fd = make_loopback_listener(&listen_addr);
    ASSERT_GE(listen_fd, 0);

    WaitGroup done(2);

    go([&done, listen_fd]() {
        sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        const int conn_fd =
            co_accept(listen_fd, reinterpret_cast<sockaddr *>(&peer_addr),
                      &peer_len, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        EXPECT_EQ(co_shutdown(conn_fd, 'b'), 0);

        errno = 0;
        EXPECT_EQ(co_send(conn_fd, "x", 1, send_nosignal_flags(), 300), -1);
        EXPECT_TRUE(is_pipe_like_errno(errno));

        char recv_probe = 0;
        const ssize_t n = co_recv(conn_fd, &recv_probe, 1, 0, 300);
        EXPECT_LE(n, 0);

        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    go([&done, listen_addr]() {
        const int conn_fd = connect_loopback(listen_addr, 1500);
        EXPECT_GE(conn_fd, 0);
        if (conn_fd < 0) {
            done.done();
            return;
        }

        char eof_probe = 0;
        EXPECT_EQ(co_recv(conn_fd, &eof_probe, 1, 0, 1500), 0);

        bool saw_pipe_error = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            errno = 0;
            const ssize_t send_rc =
                co_send(conn_fd, "x", 1, send_nosignal_flags(), 300);
            if (send_rc == -1 && is_pipe_like_errno(errno)) {
                saw_pipe_error = true;
                break;
            }

            if (send_rc >= 0) {
                EXPECT_EQ(send_rc, 1);
            }

            co_sleep_for(10);
        }
        EXPECT_TRUE(saw_pipe_error);

        EXPECT_EQ(co_close(conn_fd), 0);
        done.done();
    });

    done.wait();
    EXPECT_EQ(co_close(listen_fd), 0);
}

TEST_F(HookIntegrationTest,
       IndependentStackSocketPairRoundTripWithYieldKeepsStackLocalData) {
    co_stack_model(StackModel::kIndependent);
    co_stack_size(64 * 1024);
    co_stack_num(1);
    init(2);

    int stream_pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);
    ASSERT_EQ(::fcntl(stream_pair[0], F_SETFL,
                      ::fcntl(stream_pair[0], F_GETFL, 0) | O_NONBLOCK),
              0);
    ASSERT_EQ(::fcntl(stream_pair[1], F_SETFL,
                      ::fcntl(stream_pair[1], F_GETFL, 0) | O_NONBLOCK),
              0);

    constexpr int kRounds = 256;
    WaitGroup done(2);
    std::atomic<int> sender_ok(0);
    std::atomic<int> receiver_ok(0);
    std::atomic<int> stack_corruption(0);

    go([&done, &sender_ok, &stack_corruption, fd = stream_pair[0]]() {
        for (int i = 0; i < kRounds; ++i) {
            int stack_probe[8];
            for (int k = 0; k < 8; ++k) {
                stack_probe[k] = i + k;
            }

            char payload[16];
            for (int k = 0; k < 16; ++k) {
                payload[k] = static_cast<char>('a' + (i + k) % 26);
            }

            if ((i & 1) == 0) {
                yield();
            }

            EXPECT_EQ(co_write(fd, payload, sizeof(payload), 500),
                      static_cast<ssize_t>(sizeof(payload)));

            if ((i & 3) == 0) {
                yield();
            }

            bool corrupted = false;
            for (int k = 0; k < 8; ++k) {
                if (stack_probe[k] != i + k) {
                    corrupted = true;
                    break;
                }
            }
            if (corrupted) {
                stack_corruption.fetch_add(1, std::memory_order_relaxed);
            }

            sender_ok.fetch_add(1, std::memory_order_relaxed);
        }

        done.done();
    });

    go([&done, &receiver_ok, &stack_corruption, fd = stream_pair[1]]() {
        for (int i = 0; i < kRounds; ++i) {
            int stack_probe[8];
            for (int k = 0; k < 8; ++k) {
                stack_probe[k] = i * 10 + k;
            }

            if ((i & 1) == 1) {
                yield();
            }

            char payload[16] = {0};
            EXPECT_EQ(co_read(fd, payload, sizeof(payload), 500),
                      static_cast<ssize_t>(sizeof(payload)));

            for (int k = 0; k < 16; ++k) {
                const char expected = static_cast<char>('a' + (i + k) % 26);
                EXPECT_EQ(payload[k], expected);
            }

            bool corrupted = false;
            for (int k = 0; k < 8; ++k) {
                if (stack_probe[k] != i * 10 + k) {
                    corrupted = true;
                    break;
                }
            }
            if (corrupted) {
                stack_corruption.fetch_add(1, std::memory_order_relaxed);
            }

            receiver_ok.fetch_add(1, std::memory_order_relaxed);
        }

        done.done();
    });

    done.wait();

    EXPECT_EQ(sender_ok.load(std::memory_order_relaxed), kRounds);
    EXPECT_EQ(receiver_ok.load(std::memory_order_relaxed), kRounds);
    EXPECT_EQ(stack_corruption.load(std::memory_order_relaxed), 0);

    ::close(stream_pair[0]);
    ::close(stream_pair[1]);
}

} // namespace
} // namespace zco
