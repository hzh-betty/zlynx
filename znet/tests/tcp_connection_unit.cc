#define private public
#include "znet/tcp_connection.h"
#undef private

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "zco/sched.h"

namespace znet {
namespace {

bool is_timeout_errno(int err) {
    return err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK;
}

class TcpConnectionUnitTest : public ::testing::Test {
  protected:
    void TearDown() override { zco::shutdown(); }
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

} // namespace
} // namespace znet
