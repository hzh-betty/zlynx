#define private public
#include "znet/tcp_connection.h"
#undef private

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "zco/sched.h"
#include "znet/tls_context.h"

namespace znet {
namespace {

bool is_timeout_errno(int err) {
    return err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK;
}

class TcpConnectionUnitTest : public ::testing::Test {
  protected:
    void TearDown() override { zco::shutdown(); }
};

class FakeTlsChannel : public TlsChannel {
  public:
    bool handshake_ok = true;
    bool shutdown_called = false;
    bool call_wait_in_handshake = false;
    ssize_t read_result = 0;
    ssize_t write_result = 0;
    int forced_errno = 0;
    std::string read_payload = "tls";

    bool handshake(uint32_t, const WaitCallback &wait_callback) override {
        if (!handshake_ok) {
            errno = forced_errno == 0 ? EPROTO : forced_errno;
            return false;
        }
        if (call_wait_in_handshake && wait_callback) {
            (void)wait_callback(false, 1);
        }
        return true;
    }

    ssize_t read(void *buffer, size_t length, uint32_t,
                 const WaitCallback &) override {
        if (forced_errno != 0) {
            errno = forced_errno;
            return -1;
        }
        if (read_result <= 0) {
            return read_result;
        }
        const size_t n = std::min(static_cast<size_t>(read_result), length);
        std::memcpy(buffer, read_payload.data(), n);
        return static_cast<ssize_t>(n);
    }

    ssize_t write(const void *, size_t, uint32_t,
                  const WaitCallback &) override {
        if (forced_errno != 0) {
            errno = forced_errno;
            return -1;
        }
        return write_result;
    }

    void shutdown(uint32_t, const WaitCallback &) override {
        shutdown_called = true;
    }
};

class FakeTlsContext : public TlsContext {
  public:
    bool create_channel_ok = true;
    bool handshake_ok = true;
    bool call_wait_in_handshake = false;
    ssize_t read_result = 0;
    ssize_t write_result = 0;
    int forced_errno = 0;
    std::string payload = "tls";
    mutable FakeTlsChannel *last_channel = nullptr;

    std::unique_ptr<TlsChannel> create_server_channel(int fd) const override {
        if (!create_channel_ok || fd < 0) {
            errno = EIO;
            return nullptr;
        }

        std::unique_ptr<FakeTlsChannel> channel(new FakeTlsChannel());
        channel->handshake_ok = handshake_ok;
        channel->call_wait_in_handshake = call_wait_in_handshake;
        channel->read_result = read_result;
        channel->write_result = write_result;
        channel->forced_errno = forced_errno;
        channel->read_payload = payload;
        last_channel = channel.get();
        return std::unique_ptr<TlsChannel>(channel.release());
    }
};

TEST_F(TcpConnectionUnitTest, ReadIntoInputBufferAndFlushOutputBuffer) {
    zco::init(2);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    ASSERT_EQ(::send(pair[1], "ping", 4, 0), 4);
    ASSERT_EQ(conn->read(1024), 4);
    EXPECT_EQ(conn->input_buffer().retrieve_as_string(4), "ping");

    ASSERT_EQ(conn->send("pong", 4), 4);
    char out[8] = {0};
    ASSERT_EQ(::recv(pair[1], out, 4, 0), 4);
    EXPECT_STREQ(out, "pong");

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest,
       StateMachineTransitionsFromConnectedToDisconnected) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    EXPECT_EQ(conn->state(), TcpConnection::State::kConnected);

    conn->close();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ConcurrentSendIsSerializedByActorMailbox) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));

    const int rounds = 128;
    const std::string payload_a = "AAAA";
    const std::string payload_b = "BBBB";
    const size_t expected_bytes =
        static_cast<size_t>(rounds) * (payload_a.size() + payload_b.size());

    std::atomic<int> send_count{0};
    std::thread sender_a([&]() {
        for (int i = 0; i < rounds; ++i) {
            ASSERT_EQ(conn->send(payload_a.data(), payload_a.size()),
                      static_cast<ssize_t>(payload_a.size()));
            send_count.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread sender_b([&]() {
        for (int i = 0; i < rounds; ++i) {
            ASSERT_EQ(conn->send(payload_b.data(), payload_b.size()),
                      static_cast<ssize_t>(payload_b.size()));
            send_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::string received;
    received.reserve(expected_bytes);
    std::thread reader([&]() {
        char buf[256] = {0};
        while (received.size() < expected_bytes) {
            const ssize_t n = ::recv(pair[1], buf, sizeof(buf), 0);
            if (n > 0) {
                received.append(buf, static_cast<size_t>(n));
            }
        }
    });

    sender_a.join();
    sender_b.join();
    reader.join();

    EXPECT_EQ(send_count.load(std::memory_order_relaxed), rounds * 2);
    EXPECT_EQ(received.size(), expected_bytes);
    EXPECT_EQ(std::count(received.begin(), received.end(), 'A'),
              rounds * static_cast<int>(payload_a.size()));
    EXPECT_EQ(std::count(received.begin(), received.end(), 'B'),
              rounds * static_cast<int>(payload_b.size()));

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest,
       SendSucceedsWhenActorSchedulerIsNullInThreadContext) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    // 模拟调度器句柄不可用，验证线程上下文仍可通过 go 投递并完成发送。
    conn->actor_scheduler_ = nullptr;
    conn->actor_sched_id_ = -1;

    ASSERT_EQ(conn->send("X", 1), 1);

    char out[2] = {0};
    ASSERT_EQ(::recv(pair[1], out, 1, 0), 1);
    EXPECT_EQ(out[0], 'X');

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, WriteCompleteCallbackIsTriggeredAfterFlush) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    std::atomic<int> write_complete_count{0};
    conn->set_write_complete_callback(
        [&write_complete_count](TcpConnection::ptr c) {
            ASSERT_NE(c, nullptr);
            write_complete_count.fetch_add(1, std::memory_order_relaxed);
        });

    ASSERT_EQ(conn->send("ok", 2), 2);
    EXPECT_EQ(write_complete_count.load(std::memory_order_relaxed), 1);

    char out[4] = {0};
    ASSERT_EQ(::recv(pair[1], out, 2, 0), 2);
    EXPECT_STREQ(out, "ok");

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest,
       HighWaterMarkCallbackIsTriggeredOnThresholdCross) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    std::atomic<int> high_water_count{0};
    std::atomic<size_t> high_water_bytes{0};
    conn->set_high_water_mark_callback(
        [&high_water_count, &high_water_bytes](TcpConnection::ptr c,
                                               size_t bytes) {
            ASSERT_NE(c, nullptr);
            high_water_count.fetch_add(1, std::memory_order_relaxed);
            high_water_bytes.store(bytes, std::memory_order_relaxed);
        },
        4);

    ASSERT_EQ(conn->send("12345678", 8), 8);
    EXPECT_EQ(high_water_count.load(std::memory_order_relaxed), 1);
    EXPECT_GE(high_water_bytes.load(std::memory_order_relaxed), 8U);

    char out[16] = {0};
    ASSERT_EQ(::recv(pair[1], out, 8, 0), 8);
    EXPECT_STREQ(out, "12345678");

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ShutdownClosesConnectionIdempotently) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    ASSERT_EQ(conn->send("bye", 3), 3);
    conn->shutdown();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    // 再次调用应保持幂等。
    conn->shutdown();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ReadTimeoutIsReportedAsEtimedout) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    const auto started = std::chrono::steady_clock::now();
    errno = 0;
    const ssize_t n = conn->read(64, 30);
    const auto ended = std::chrono::steady_clock::now();

    EXPECT_EQ(n, -1);
    EXPECT_TRUE(is_timeout_errno(errno));
    EXPECT_GE(
        std::chrono::duration_cast<std::chrono::milliseconds>(ended - started)
            .count(),
        20);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ConnectionWriteTimeoutCanBeConfigured) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    conn->set_write_timeout(123);
    EXPECT_EQ(conn->write_timeout(), 123U);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendSucceedsInsideCoroutineContext) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    zco::WaitGroup done(1);
    zco::go([&]() {
        EXPECT_EQ(conn->send("fast", 4), 4);
        done.done();
    });
    done.wait();

    char out[8] = {0};
    ASSERT_EQ(::recv(pair[1], out, 4, 0), 4);
    EXPECT_STREQ(out, "fast");
    EXPECT_EQ(conn->output_buffer().readable_bytes(), 0U);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendRejectsNullDataAndAcceptsZeroLength) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_EQ(conn->send(nullptr, 1), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(conn->send("x", 0), 0);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ReadAndFlushRejectDisconnectedState) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    conn->set_state(TcpConnection::State::kDisconnected);
    errno = 0;
    EXPECT_EQ(conn->read(16, 10), -1);
    EXPECT_EQ(errno, EBADF);

    errno = 0;
    EXPECT_EQ(conn->flush_output(10), -1);
    EXPECT_EQ(errno, EBADF);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ReadRejectsZeroReadBytes) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_EQ(conn->read(0, 10), -1);
    EXPECT_EQ(errno, EINVAL);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, EnableTlsServerRejectsInvalidArguments) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_FALSE(conn->enable_tls_server(nullptr, 100));
    EXPECT_EQ(errno, EINVAL);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, EnableTlsServerCreateChannelFailureIsReported) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto ctx = std::make_shared<FakeTlsContext>();
    ctx->create_channel_ok = false;
    errno = 0;
    EXPECT_FALSE(conn->enable_tls_server(ctx, 100));
    EXPECT_EQ(errno, EIO);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, EnableTlsServerHandshakeFailureIsReported) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto ctx = std::make_shared<FakeTlsContext>();
    ctx->handshake_ok = false;
    ctx->forced_errno = ETIMEDOUT;
    errno = 0;
    EXPECT_FALSE(conn->enable_tls_server(ctx, 100));
    EXPECT_EQ(errno, ETIMEDOUT);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, EnableTlsServerReadAndWritePathsUseTlsChannel) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto ctx = std::make_shared<FakeTlsContext>();
    ctx->read_result = 3;
    ctx->payload = "abc";
    ctx->write_result = 2;

    ASSERT_TRUE(conn->enable_tls_server(ctx, 100));
    ASSERT_NE(ctx->last_channel, nullptr);

    ASSERT_EQ(conn->read(16, 10), 3);
    EXPECT_EQ(conn->input_buffer().retrieve_all_as_string(), "abc");

    conn->output_buffer().append("xy", 2);
    ASSERT_EQ(conn->flush_output(10), 2);
    EXPECT_EQ(conn->output_buffer().readable_bytes(), 0U);

    conn->shutdown();
    EXPECT_TRUE(ctx->last_channel->shutdown_called);
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, EnableTlsServerReturnsTrueWhenAlreadyEnabled) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto ctx = std::make_shared<FakeTlsContext>();
    ctx->call_wait_in_handshake = true;
    ASSERT_TRUE(conn->enable_tls_server(ctx, 100));
    ASSERT_TRUE(conn->enable_tls_server(ctx, 100));

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, FlushOutputTlsFailurePropagatesError) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto fake = std::unique_ptr<FakeTlsChannel>(new FakeTlsChannel());
    fake->forced_errno = EIO;
    conn->tls_channel_ = std::unique_ptr<TlsChannel>(fake.release());
    conn->output_buffer().append("data", 4);

    errno = 0;
    EXPECT_EQ(conn->flush_output(10), -1);
    EXPECT_EQ(errno, EIO);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, CloseAndShutdownRemainIdempotentWhenDisconnected) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    conn->close();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    conn->close();
    conn->shutdown();
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ConstructorHandlesNullSocketAndDispatchNullEvent) {
    auto conn = std::make_shared<TcpConnection>(Socket::ptr{});
    ASSERT_NE(conn, nullptr);
    EXPECT_EQ(conn->fd(), -1);
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    errno = 0;
    EXPECT_EQ(conn->dispatch_event_and_wait(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(TcpConnectionUnitTest, ReentrantDispatchPathWorksInsideInlineActor) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    zco::WaitGroup done(1);
    zco::go([&]() {
        ASSERT_TRUE(conn->try_begin_inline_actor());
        auto event =
            std::make_shared<TcpConnection::Event>(TcpConnection::EventType::kClose);
        EXPECT_EQ(conn->dispatch_event_and_wait(event), 0);
        conn->finish_inline_actor();
        done.done();
    });
    done.wait();

    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendInternalReturnsErrorOutsideCoroutineContext) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_EQ(conn->send_internal("x", 1, 10), -1);
    EXPECT_EQ(errno, EPERM);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendInternalValidatesStateAndLength) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    conn->set_state(TcpConnection::State::kDisconnected);
    errno = 0;
    EXPECT_EQ(conn->send_internal("x", 1, 10), -1);
    EXPECT_EQ(errno, EBADF);

    conn->set_state(TcpConnection::State::kConnected);
    EXPECT_EQ(conn->send_internal("x", 0, 10), 0);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendInternalClosesDisconnectingConnectionAfterFlush) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    conn->set_state(TcpConnection::State::kDisconnecting);
    conn->output_buffer().append("a", 1);

    zco::WaitGroup done(1);
    zco::go([&]() {
        EXPECT_EQ(conn->send_internal("b", 1, 200), 1);
        done.done();
    });
    done.wait();

    char out[4] = {0};
    ASSERT_EQ(::recv(pair[1], out, 2, 0), 2);
    EXPECT_STREQ(out, "ab");
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendInternalFastPathClosesDisconnectingConnection) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);
    conn->set_state(TcpConnection::State::kDisconnecting);

    zco::WaitGroup done(1);
    zco::go([&]() {
        EXPECT_EQ(conn->send_internal("q", 1, 200), 1);
        done.done();
    });
    done.wait();

    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, TlsReadReturnsZeroAndFlushHandlesZeroWrite) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    auto ctx = std::make_shared<FakeTlsContext>();
    ctx->read_result = 0;
    ctx->write_result = 0;
    ASSERT_TRUE(conn->enable_tls_server(ctx, 100));

    EXPECT_EQ(conn->read(8, 10), 0);
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

    conn->set_state(TcpConnection::State::kConnected);
    conn->output_buffer().append("data", 4);
    EXPECT_EQ(conn->flush_output(10), 0);
    EXPECT_EQ(conn->output_buffer().readable_bytes(), 4U);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, DispatchUsesInlineAndSchedulerWorkerPaths) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_EQ(conn->read(0, 1), -1);
    EXPECT_EQ(errno, EINVAL);

    zco::WaitGroup done(1);
    zco::go([&]() {
        conn->actor_sched_id_ = zco::sched_id();
        auto event =
            std::make_shared<TcpConnection::Event>(TcpConnection::EventType::kRead);
        event->max_read_bytes = 0;
        event->timeout_ms = 1;
        errno = 0;
        EXPECT_EQ(conn->dispatch_event_and_wait(event), -1);
        EXPECT_EQ(errno, EINVAL);
        done.done();
    });
    done.wait();

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, TryBeginInlineActorHandlesSchedMismatchAndBusyActor) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    zco::WaitGroup done(1);
    zco::go([&]() {
        conn->actor_sched_id_ = zco::sched_id() + 100;
        EXPECT_FALSE(conn->try_begin_inline_actor());

        conn->actor_sched_id_ = zco::sched_id();
        conn->actor_running_ = true;
        EXPECT_FALSE(conn->try_begin_inline_actor());
        conn->actor_running_ = false;
        done.done();
    });
    done.wait();

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ReadWriteTlsInternalValidationAndIoWaitPaths) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    errno = 0;
    EXPECT_EQ(conn->read_tls_internal(8, 10), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(conn->write_tls_internal(nullptr, 1, 10), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(conn->write_tls_internal("x", 0, 10), -1);
    EXPECT_EQ(errno, EINVAL);

    zco::WaitGroup done(1);
    std::atomic<bool> write_ready{false};
    std::atomic<bool> read_wait_returned{false};
    zco::go([&]() {
        write_ready.store(conn->wait_tls_io(true, 20), std::memory_order_release);
        read_wait_returned.store(conn->wait_tls_io(false, 20),
                                 std::memory_order_release);
        done.done();
    });
    done.wait();

    EXPECT_TRUE(write_ready.load(std::memory_order_acquire));
    (void)read_wait_returned.load(std::memory_order_acquire);

    conn->close();
    ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, InlineActorPathsInFlushShutdownAndClose) {
    zco::init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn =
        std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    zco::WaitGroup done(1);
    zco::go([&]() {
        conn->set_write_timeout(200);
        conn->output_buffer().append("xy", 2);
        EXPECT_EQ(conn->flush_output(), 2);
        conn->shutdown();
        conn->close();
        done.done();
    });
    done.wait();

    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    ::close(pair[1]);
}

} // namespace
} // namespace znet
