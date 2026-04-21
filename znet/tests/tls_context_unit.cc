#include "znet/tls_context.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace znet {
namespace {

struct CertFiles {
    std::string cert;
    std::string key;
    std::string mismatch_key;
};

bool command_ok(const std::string &cmd) { return std::system(cmd.c_str()) == 0; }

CertFiles create_cert_files() {
    char dir_template[] = "/tmp/znet-tls-XXXXXX";
    char *dir = ::mkdtemp(dir_template);
    EXPECT_NE(dir, nullptr);

    CertFiles files;
    files.cert = std::string(dir) + "/cert.pem";
    files.key = std::string(dir) + "/key.pem";
    files.mismatch_key = std::string(dir) + "/mismatch_key.pem";

    const std::string cert_cmd =
        "openssl req -x509 -nodes -newkey rsa:2048 -days 1 "
        "-subj '/CN=localhost' -keyout " +
        files.key + " -out " + files.cert + " >/dev/null 2>&1";
    EXPECT_TRUE(command_ok(cert_cmd));

    const std::string mismatch_cmd =
        "openssl genrsa -out " + files.mismatch_key + " 2048 >/dev/null 2>&1";
    EXPECT_TRUE(command_ok(mismatch_cmd));
    return files;
}

bool wait_fd_event(int fd, bool wait_for_write, uint32_t timeout_ms) {
    pollfd pfd;
    std::memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = static_cast<short>(wait_for_write ? POLLOUT : POLLIN);
    const int wait_ms = timeout_ms == 0 ? -1 : static_cast<int>(timeout_ms);
    const int rc = ::poll(&pfd, 1, wait_ms);
    if (rc > 0) {
        return true;
    }
    if (rc == 0) {
        errno = ETIMEDOUT;
    }
    return false;
}

std::pair<int, int> create_connected_tcp_pair() {
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return {-1, -1};
    }

    int one = 1;
    if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) !=
        0) {
        ::close(listener);
        return {-1, -1};
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        ::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(listener, 4) != 0) {
        ::close(listener);
        return {-1, -1};
    }

    sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound),
                      &bound_len) != 0) {
        ::close(listener);
        return {-1, -1};
    }

    int server_fd = -1;
    std::thread accept_thread([&]() {
        server_fd = ::accept(listener, nullptr, nullptr);
    });

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0 ||
        ::connect(client_fd, reinterpret_cast<sockaddr *>(&bound),
                  sizeof(bound)) != 0) {
        if (client_fd >= 0) {
            ::close(client_fd);
        }
        ::close(listener);
        accept_thread.join();
        if (server_fd >= 0) {
            ::close(server_fd);
        }
        return {-1, -1};
    }

    accept_thread.join();
    ::close(listener);

    return {server_fd, client_fd};
}

class TlsContextUnitTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const int rc = std::system("openssl version >/dev/null 2>&1");
        if (rc != 0) {
            GTEST_SKIP() << "openssl command is required for tls tests";
        }
    }
};

TEST_F(TlsContextUnitTest, CreateServerContextRejectsInvalidCertificatePaths) {
    std::string error;
    auto ctx = create_server_tls_context_openssl(
        "/tmp/not-exist-cert.pem", "/tmp/not-exist-key.pem", &error);

    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(TlsContextUnitTest, CreateServerContextFailureAllowsNullErrorPointer) {
    auto ctx = create_server_tls_context_openssl("/tmp/not-exist-cert.pem",
                                                 "/tmp/not-exist-key.pem",
                                                 nullptr);
    EXPECT_EQ(ctx, nullptr);
}

TEST_F(TlsContextUnitTest, RejectsMismatchedCertificateAndPrivateKey) {
    CertFiles files = create_cert_files();

    std::string error;
    auto ctx = create_server_tls_context_openssl(files.cert, files.mismatch_key,
                                                 &error);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(TlsContextUnitTest, CreateContextAndChannelValidation) {
    CertFiles files = create_cert_files();

    std::string error;
    auto ctx = create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(ctx, nullptr) << error;

    errno = 0;
    auto invalid = ctx->create_server_channel(-1);
    EXPECT_EQ(invalid, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(TlsContextUnitTest, ClosedFdLeadsToTlsChannelFailureOnCreateOrHandshake) {
    CertFiles files = create_cert_files();
    std::string error;
    auto ctx = create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(ctx, nullptr) << error;

    int closed_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(closed_fd, 0);
    ASSERT_EQ(::close(closed_fd), 0);

    errno = 0;
    auto ch = ctx->create_server_channel(closed_fd);
    if (!ch) {
        EXPECT_EQ(errno, EIO);
        return;
    }

    errno = 0;
    const bool ok = ch->handshake(10, TlsChannel::WaitCallback{});
    EXPECT_FALSE(ok);
    EXPECT_NE(errno, 0);
}

TEST_F(TlsContextUnitTest, HandshakeReadWriteAndShutdownSucceed) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::atomic<bool> server_handshake_ok{false};
    std::thread server_thread([&]() {
        server_handshake_ok.store(
            channel->handshake(
                2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                    return wait_fd_event(server_fd, wait_for_write, timeout_ms);
                }),
            std::memory_order_release);
    });

    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();
    ASSERT_TRUE(server_handshake_ok.load(std::memory_order_acquire));

    ASSERT_EQ(SSL_write(client_ssl.get(), "ping", 4), 4);
    char server_buf[16] = {0};
    ASSERT_EQ(channel->read(
                  server_buf, sizeof(server_buf), 1000,
                  [&](bool wait_for_write, uint32_t timeout_ms) {
                      return wait_fd_event(server_fd, wait_for_write, timeout_ms);
                  }),
              4);
    EXPECT_STREQ(server_buf, "ping");

    ASSERT_EQ(channel->write(
                  "pong", 4, 1000, [&](bool wait_for_write, uint32_t timeout_ms) {
                      return wait_fd_event(server_fd, wait_for_write, timeout_ms);
                  }),
              4);
    char client_buf[16] = {0};
    ASSERT_EQ(SSL_read(client_ssl.get(), client_buf, sizeof(client_buf)), 4);
    EXPECT_STREQ(client_buf, "pong");

    channel->shutdown(500, [&](bool wait_for_write, uint32_t timeout_ms) {
        return wait_fd_event(server_fd, wait_for_write, timeout_ms);
    });

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, ChannelReadWriteValidateArguments) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    errno = 0;
    EXPECT_EQ(channel->read(nullptr, 8, 100, TlsChannel::WaitCallback{}), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(channel->write(nullptr, 1, 100, TlsChannel::WaitCallback{}), -1);
    EXPECT_EQ(errno, EINVAL);

    const char dummy = 'x';
    EXPECT_EQ(channel->write(&dummy, 0, 100, TlsChannel::WaitCallback{}), 0);

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, HandshakeTimeoutPathSetsEtimedout) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    const int flags = ::fcntl(pair[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(pair[0], F_SETFL, flags | O_NONBLOCK), 0);
    auto channel = server_ctx->create_server_channel(pair[0]);
    ASSERT_NE(channel, nullptr);

    errno = 0;
    const bool ok = channel->handshake(10, [](bool, uint32_t) {
        errno = 0;
        return false;
    });
    EXPECT_FALSE(ok);
    EXPECT_EQ(errno, ETIMEDOUT);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(TlsContextUnitTest, HandshakeTimeoutWithoutWaitCallbackSetsEtimedout) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    const int flags = ::fcntl(pair[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(pair[0], F_SETFL, flags | O_NONBLOCK), 0);
    auto channel = server_ctx->create_server_channel(pair[0]);
    ASSERT_NE(channel, nullptr);

    errno = 0;
    const bool ok = channel->handshake(10, TlsChannel::WaitCallback{});
    EXPECT_FALSE(ok);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(TlsContextUnitTest,
       HandshakeFailsWhenPeerClosesBeforeTlsClientHandshake) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    linger reset_opt{1, 0};
    ASSERT_EQ(::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset_opt,
                           sizeof(reset_opt)),
              0);
    ASSERT_EQ(::close(client_fd), 0);

    errno = 0;
    const bool ok = channel->handshake(
        100, [&](bool wait_for_write, uint32_t timeout_ms) {
            return wait_fd_event(server_fd, wait_for_write, timeout_ms);
        });
    EXPECT_FALSE(ok);
    EXPECT_NE(errno, 0);

    ::close(server_fd);
}

TEST_F(TlsContextUnitTest, ReadReturnsZeroAfterPeerCloseNotify) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    ASSERT_GE(SSL_shutdown(client_ssl.get()), 0);
    char buffer[8] = {0};
    EXPECT_EQ(channel->read(
                  buffer, sizeof(buffer), 1000,
                  [&](bool wait_for_write, uint32_t timeout_ms) {
                      return wait_fd_event(server_fd, wait_for_write, timeout_ms);
                  }),
              0);

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, ReadAndWriteReportTimeoutOrPeerClosureErrors) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    const int flags = ::fcntl(server_fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(server_fd, F_SETFL, flags | O_NONBLOCK), 0);

    errno = 0;
    char buffer[8] = {0};
    EXPECT_EQ(channel->read(buffer, sizeof(buffer), 30,
                            [](bool, uint32_t) {
                                errno = 0;
                                return false;
                            }),
              -1);
    EXPECT_EQ(errno, ETIMEDOUT);

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, ReadTimeoutWithoutWaitCallbackSetsEtimedout) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    const int flags = ::fcntl(server_fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(server_fd, F_SETFL, flags | O_NONBLOCK), 0);

    errno = 0;
    char buffer[8] = {0};
    EXPECT_EQ(channel->read(buffer, sizeof(buffer), 20, TlsChannel::WaitCallback{}),
              -1);
    EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN);

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, CreateServerContextRejectsInvalidPrivateKeyPath) {
    CertFiles files = create_cert_files();

    std::string error;
    auto ctx = create_server_tls_context_openssl(files.cert,
                                                 "/tmp/not-exist-key.pem",
                                                 &error);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(TlsContextUnitTest,
       CreateServerContextPrivateKeyFailureAllowsNullErrorPointer) {
    CertFiles files = create_cert_files();

    auto ctx = create_server_tls_context_openssl(files.cert,
                                                 "/tmp/not-exist-key.pem",
                                                 nullptr);
    EXPECT_EQ(ctx, nullptr);
}

TEST_F(TlsContextUnitTest,
       MismatchedCertificateAndKeyFailureAllowsNullErrorPointer) {
    CertFiles files = create_cert_files();

    auto ctx = create_server_tls_context_openssl(files.cert, files.mismatch_key,
                                                 nullptr);
    EXPECT_EQ(ctx, nullptr);
}

TEST_F(TlsContextUnitTest, InvalidFdChannelEitherFailsCreateOrFailsHandshake) {
    CertFiles files = create_cert_files();
    std::string error;
    auto ctx = create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(ctx, nullptr) << error;

    constexpr int kClearlyInvalidFd = 1 << 20;
    errno = 0;
    auto channel = ctx->create_server_channel(kClearlyInvalidFd);
    if (!channel) {
        EXPECT_EQ(errno, EIO);
        return;
    }

    errno = 0;
    EXPECT_FALSE(channel->handshake(10, TlsChannel::WaitCallback{}));
    EXPECT_NE(errno, 0);
}

TEST_F(TlsContextUnitTest, ReadReportsErrorAfterAbruptPeerReset) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    linger reset_opt{1, 0};
    ASSERT_EQ(::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset_opt,
                           sizeof(reset_opt)),
              0);
    ASSERT_EQ(::close(client_fd), 0);

    errno = 0;
    char buffer[8] = {0};
    EXPECT_EQ(channel->read(
                  buffer, sizeof(buffer), 100,
                  [&](bool wait_for_write, uint32_t timeout_ms) {
                      return wait_fd_event(server_fd, wait_for_write, timeout_ms);
                  }),
              -1);
    EXPECT_NE(errno, 0);

    ::close(server_fd);
}

TEST_F(TlsContextUnitTest, WriteWithNoWaitCallbackCanTimeoutWithPartialProgress) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    int sndbuf = 1024;
    ASSERT_EQ(::setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
                           sizeof(sndbuf)),
              0);
    const int flags = ::fcntl(server_fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(server_fd, F_SETFL, flags | O_NONBLOCK), 0);

    std::vector<char> chunk(256 * 1024, 'x');
    ssize_t last_write = static_cast<ssize_t>(chunk.size());
    bool backpressured = false;
    for (int i = 0; i < 256; ++i) {
        errno = 0;
        last_write = channel->write(chunk.data(), chunk.size(), 10,
                                    TlsChannel::WaitCallback{});
        if (last_write < static_cast<ssize_t>(chunk.size())) {
            backpressured = true;
            break;
        }
    }
    EXPECT_TRUE(backpressured);
    EXPECT_TRUE(last_write >= -1);
    if (last_write < 0) {
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == EIO);
    }

    ::close(server_fd);
    ::close(client_fd);
}

TEST_F(TlsContextUnitTest, WriteReportsErrorAfterAbruptPeerReset) {
    CertFiles files = create_cert_files();
    std::string error;
    auto server_ctx =
        create_server_tls_context_openssl(files.cert, files.key, &error);
    ASSERT_NE(server_ctx, nullptr) << error;

    auto pair = create_connected_tcp_pair();
    const int server_fd = pair.first;
    const int client_fd = pair.second;
    ASSERT_GE(server_fd, 0);
    ASSERT_GE(client_fd, 0);

    auto channel = server_ctx->create_server_channel(server_fd);
    ASSERT_NE(channel, nullptr);

    SSL_CTX *client_ctx_raw = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx_raw, nullptr);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_ctx(client_ctx_raw,
                                                                  &SSL_CTX_free);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);
    SSL *client_ssl_raw = SSL_new(client_ctx.get());
    ASSERT_NE(client_ssl_raw, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client_ssl(client_ssl_raw,
                                                         &SSL_free);
    ASSERT_EQ(SSL_set_fd(client_ssl.get(), client_fd), 1);

    std::thread server_thread([&]() {
        ASSERT_TRUE(channel->handshake(
            2000, [&](bool wait_for_write, uint32_t timeout_ms) {
                return wait_fd_event(server_fd, wait_for_write, timeout_ms);
            }));
    });
    ASSERT_EQ(SSL_connect(client_ssl.get()), 1);
    server_thread.join();

    linger reset_opt{1, 0};
    ASSERT_EQ(::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset_opt,
                           sizeof(reset_opt)),
              0);
    ASSERT_EQ(::close(client_fd), 0);

    errno = 0;
    const ssize_t written = channel->write(
        "ping", 4, 50, [&](bool wait_for_write, uint32_t timeout_ms) {
            return wait_fd_event(server_fd, wait_for_write, timeout_ms);
        });
    EXPECT_LE(written, 4);
    if (written < 4) {
        EXPECT_NE(errno, 0);
    }

    ::close(server_fd);
}

} // namespace
} // namespace znet
