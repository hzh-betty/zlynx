#include "../support/network_fixture.h"
#include "zco/sync/event.h"
#include "znet/server/tcp_server.h"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <thread>

using namespace znet;
using namespace std::chrono_literals;

namespace {
struct CertificateFiles {
    CertificateFiles() {
        char pattern[] = "/tmp/znet-credentials-XXXXXX";
        const char *directory = ::mkdtemp(pattern);
        if (!directory)
            throw std::runtime_error("mkdtemp");
        path = directory;
        cert = path + "/cert.pem";
        key = path + "/key.pem";
        const std::string command =
            "openssl req -x509 -nodes -newkey rsa:2048 -days 1 "
            "-subj '/CN=localhost' -keyout " +
            key + " -out " + cert + " >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0) {
            std::filesystem::remove_all(path);
            throw std::runtime_error("openssl certificate generation failed");
        }
    }

    ~CertificateFiles() { std::filesystem::remove_all(path); }

    std::string path, cert, key;
};

struct TlsPair {
    explicit TlsPair(std::chrono::milliseconds timeout = {})
        : runtime(zco::RuntimeOptions{1}) {
        CertificateFiles files;
        auto credentials = TlsCredentials::load(files.cert, files.key);
        if (!credentials)
            throw std::runtime_error(credentials.error().message());
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            throw std::runtime_error("socketpair");
        peer = fds[1];
        timeval limit{2, 0};
        ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        ::setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        auto stream = credentials.value().make_stream(test::adopt(fds[0]));
        if (!stream)
            throw std::runtime_error(stream.error().message());
        server = std::make_shared<Connection>(std::move(stream).value(),
                                              runtime.executor(0), timeout);
        client_context.reset(SSL_CTX_new(TLS_client_method()));
        if (!client_context)
            throw std::runtime_error("client TLS context");
        SSL_CTX_set_verify(client_context.get(), SSL_VERIFY_NONE, nullptr);
        client.reset(SSL_new(client_context.get()));
        SSL_set_fd(client.get(), peer);
    }

    ~TlsPair() {
        (void)server->close();
        client.reset();
        ::close(peer);
    }

    bool handshake() {
        bool started = false;
        std::thread accept(
            [&] { started = bool(server->start(zco::Deadline::after(1s))); });
        const bool connected = SSL_connect(client.get()) == 1;
        accept.join();
        return started && connected;
    }

    zco::Runtime runtime;
    Connection::ptr server;
    int peer = -1;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> client_context{
        nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> client{nullptr, SSL_free};
};
} // namespace

TEST(TlsStreamTest, CredentialsFailExplicitlyAndRejectMismatchedKeys) {
    auto missing = TlsCredentials::load("/missing/cert", "/missing/key");
    ASSERT_FALSE(missing);
    EXPECT_FALSE(missing.error().detail.empty());
    EXPECT_FALSE(TlsCredentials::load("", ""));
    CertificateFiles first, second;
    EXPECT_FALSE(TlsCredentials::load(first.cert, second.key));
}

TEST(TlsStreamTest, RoundTripEofAndShutdownUseTheSameConnectionContract) {
    TlsPair pair;
    ASSERT_TRUE(pair.handshake());
    EXPECT_TRUE(pair.server->encrypted());
    ASSERT_EQ(SSL_write(pair.client.get(), "ping", 4), 4);
    ByteBuffer input;
    auto received = pair.server->read(input, 4, zco::Deadline::after(1s));
    ASSERT_TRUE(received);
    EXPECT_EQ(input.view(), "ping");
    ASSERT_TRUE(pair.server->send("pong"));
    char bytes[8]{};
    ASSERT_EQ(SSL_read(pair.client.get(), bytes, sizeof(bytes)), 4);
    EXPECT_EQ(std::string(bytes, 4), "pong");
    ASSERT_GE(SSL_shutdown(pair.client.get()), 0);
    EXPECT_TRUE(pair.server->read(input, 4, zco::Deadline::after(1s)).eof);
    EXPECT_TRUE(pair.server->shutdown());
    EXPECT_EQ(pair.server->native_handle(), -1);
}

TEST(TlsStreamTest, ParkedTlsReadDoesNotStallWriteOnItsWorker) {
    TlsPair pair;
    ASSERT_TRUE(pair.handshake());
    zco::Event entered(true), written(true);
    ByteBuffer input;
    auto read = pair.runtime.spawn([&] {
        entered.signal();
        EXPECT_TRUE(pair.server->read(input, 4, zco::Deadline::after(1s)));
    });
    auto write = pair.runtime.spawn([&] {
        entered.wait().value();
        EXPECT_TRUE(pair.server->send("out", 100ms));
        written.signal();
    });
    ASSERT_TRUE(read);
    ASSERT_TRUE(write);
    EXPECT_TRUE(written.wait(zco::Deadline::after(500ms)));
    char bytes[4]{};
    EXPECT_EQ(SSL_read(pair.client.get(), bytes, sizeof(bytes)), 3);
    EXPECT_EQ(std::string(bytes, 3), "out");
    EXPECT_EQ(SSL_write(pair.client.get(), "ping", 4), 4);
    EXPECT_TRUE(read.value().join());
    EXPECT_TRUE(write.value().join());
    EXPECT_EQ(input.view(), "ping");
}

TEST(TlsStreamTest, HandshakeTimeoutClosesTransport) {
    TlsPair pair;
    auto result = pair.server->start(zco::Deadline::after(20ms));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, std::errc::timed_out);
    EXPECT_EQ(pair.server->native_handle(), -1);
    EXPECT_FALSE(pair.server->connected());
}

TEST(TlsStreamTest, WriteBackpressureHonorsOneTimeoutAndRetainsProgress) {
    TlsPair pair(20ms);
    ASSERT_TRUE(pair.handshake());
    const int size = 1024;
    ASSERT_EQ(::setsockopt(pair.server->native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &size, sizeof(size)),
              0);
    const std::string payload(1024 * 1024, 'x');
    const auto begin = std::chrono::steady_clock::now();
    auto sent = pair.server->send(payload);
    ASSERT_FALSE(sent);
    EXPECT_LT(sent.bytes, payload.size());
    EXPECT_EQ(sent.error.code, std::errc::timed_out);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 500ms);
    EXPECT_FALSE(pair.server->connected());
}

TEST(TlsStreamTest,
     ServerStopCancelsUnboundedHandshakeBeforeBusinessCallbacks) {
    CertificateFiles files;
    auto credentials = TlsCredentials::load(files.cert, files.key);
    ASSERT_TRUE(credentials);
    zco::Runtime runtime(zco::RuntimeOptions{1});
    ServerOptions options;
    options.handshake_timeout = 0ms;
    options.tls =
        std::make_shared<const TlsCredentials>(std::move(credentials).value());
    std::atomic<int> opened{0};
    TcpServer server(
        runtime, Endpoint::ipv4("127.0.0.1", 0).value(),
        [&](const Connection::ptr &) {
            ++opened;
            return SessionCallbacks{};
        },
        options);
    ASSERT_TRUE(server.start());
    auto client = test::connect_to(server.local_endpoint().value());
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!server.active_connections() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_EQ(server.active_connections(), 1u);
    const auto begin = std::chrono::steady_clock::now();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 500ms);
    EXPECT_EQ(opened, 0);
    EXPECT_EQ(server.active_connections(), 0u);
}
