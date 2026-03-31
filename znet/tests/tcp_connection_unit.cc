#include "znet/tcp_connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <gtest/gtest.h>

#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"

namespace znet {
namespace {

class TcpConnectionUnitTest : public ::testing::Test {
 protected:
  void TearDown() override { zcoroutine::shutdown(); }
};

class LegacyMemoryStream : public Stream {
 protected:
  ssize_t do_read(void* buffer, size_t length, uint32_t /*timeout_ms*/) override {
    if (!buffer || length == 0 || data_.empty()) {
      errno = EAGAIN;
      return -1;
    }

    const size_t n = std::min(length, data_.size());
    std::memcpy(buffer, data_.data(), n);
    data_.erase(0, n);
    return static_cast<ssize_t>(n);
  }

  ssize_t do_write(const void* buffer, size_t length,
                   uint32_t /*timeout_ms*/) override {
    if (!buffer && length > 0) {
      errno = EINVAL;
      return -1;
    }

    data_.append(static_cast<const char*>(buffer), length);
    return static_cast<ssize_t>(length);
  }

  size_t pending_bytes() const override { return data_.size(); }

 private:
  std::string data_;
};

TEST_F(TcpConnectionUnitTest, ReadIntoInputBufferAndFlushOutputBuffer) {
  zcoroutine::init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  conn->set_streams(read_stream, write_stream);
  zcoroutine::WaitGroup done(2);

  zcoroutine::go([peer = pair[1], &done]() {
    ASSERT_EQ(::send(peer, "ping", 4, 0), 4);
    done.done();
  });

  zcoroutine::go([conn, peer = pair[1], &done]() {
    conn->bind_to_current_loop();
    EXPECT_EQ(conn->read(1024), 4);
    char in[8] = {0};
    ASSERT_EQ(conn->read_stream()->read(in, 4), 4);
    EXPECT_STREQ(in, "ping");

    ASSERT_EQ(conn->write_stream()->write("pong", 4), 4);
    EXPECT_EQ(conn->flush_output(), 4);

    char out[8] = {0};
    ASSERT_EQ(::recv(peer, out, 4, 0), 4);
    EXPECT_STREQ(out, "pong");
    done.done();
  });

  done.wait();
  ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, ReadOnceDoesNotFallbackForLegacyStream) {
  zcoroutine::init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto legacy = std::make_shared<LegacyMemoryStream>();
  conn->set_streams(legacy, std::make_shared<BufferStream>());

  zcoroutine::WaitGroup done(1);
  zcoroutine::go([conn, &done]() {
    conn->bind_to_current_loop();
    EXPECT_EQ(conn->read(1024), 0);
    EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);
    done.done();
  });

  ASSERT_EQ(::send(pair[1], "ping", 4, 0), 4);
  done.wait();

  ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, StateMachineTransitionsFromConnectingToDisconnected) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  EXPECT_EQ(conn->state(), TcpConnection::State::kConnecting);

  zcoroutine::init(1);
  zcoroutine::WaitGroup done(1);
  zcoroutine::go([conn, &done]() {
    conn->bind_to_current_loop();
    done.done();
  });
  done.wait();

  EXPECT_EQ(conn->state(), TcpConnection::State::kConnected);
  conn->close();
  EXPECT_EQ(conn->state(), TcpConnection::State::kDisconnected);

  ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, SendCalledFromOtherThreadDispatchesToOwnerLoop) {
  zcoroutine::init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  conn->set_streams(read_stream, write_stream);

  zcoroutine::Scheduler* owner = zcoroutine::next_sched();
  ASSERT_NE(owner, nullptr);

  std::atomic<bool> bound{false};
  owner->go([conn, owner, &bound]() {
    conn->bind_to_loop(owner);
    bound.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 100 && !bound.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(bound.load(std::memory_order_acquire));

  ASSERT_EQ(conn->send("ping", 4), 4);

  char out[8] = {0};
  ssize_t n = -1;
  for (int i = 0; i < 100; ++i) {
    n = ::recv(pair[1], out, 4, MSG_DONTWAIT);
    if (n == 4) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(n, 4);
  EXPECT_STREQ(out, "ping");

  conn->close();
  ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, WriteCompleteCallbackIsTriggeredAfterFlush) {
  zcoroutine::init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  conn->set_streams(read_stream, write_stream);

  std::atomic<int> write_complete_count{0};
  zcoroutine::WaitGroup done(1);

  zcoroutine::go([conn, &write_complete_count, &done]() {
    conn->bind_to_current_loop();
    conn->set_write_complete_callback([&write_complete_count](TcpConnection::ptr c) {
      ASSERT_NE(c, nullptr);
      write_complete_count.fetch_add(1);
    });

    ASSERT_EQ(conn->send("ok", 2), 2);
    done.done();
  });

  done.wait();
  EXPECT_EQ(write_complete_count.load(), 1);

  char out[4] = {0};
  ASSERT_EQ(::recv(pair[1], out, 2, 0), 2);
  EXPECT_STREQ(out, "ok");

  conn->close();
  ::close(pair[1]);
}

TEST_F(TcpConnectionUnitTest, HighWaterMarkCallbackIsTriggeredOnThresholdCross) {
  zcoroutine::init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  conn->set_streams(read_stream, write_stream);

  std::atomic<int> high_water_count{0};
  std::atomic<size_t> high_water_bytes{0};
  zcoroutine::WaitGroup done(1);

  zcoroutine::go([conn, &high_water_count, &high_water_bytes, &done]() {
    conn->bind_to_current_loop();
    conn->set_high_water_mark_callback(
        [&high_water_count, &high_water_bytes](TcpConnection::ptr c, size_t bytes) {
          ASSERT_NE(c, nullptr);
          high_water_count.fetch_add(1);
          high_water_bytes.store(bytes);
        },
        4);

    ASSERT_EQ(conn->send("12345678", 8), 8);
    done.done();
  });

  done.wait();
  EXPECT_EQ(high_water_count.load(), 1);
  EXPECT_GE(high_water_bytes.load(), 8U);

  char out[16] = {0};
  ASSERT_EQ(::recv(pair[1], out, 8, 0), 8);
  EXPECT_STREQ(out, "12345678");

  conn->close();
  ::close(pair[1]);
}

}  // namespace
}  // namespace znet
