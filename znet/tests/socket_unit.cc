#include "znet/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>

#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"

namespace znet {
namespace {

class SocketCoroutineUnitTest : public ::testing::Test {
 protected:
  void TearDown() override { zcoroutine::shutdown(); }
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
  zcoroutine::init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  Socket writer(pair[0]);
  Socket reader(pair[1]);
  zcoroutine::WaitGroup done(2);

  zcoroutine::go([&writer, &done]() {
    const char payload[] = "ping";
    EXPECT_EQ(writer.send(payload, sizeof(payload) - 1, 0, 200),
              static_cast<ssize_t>(sizeof(payload) - 1));
    done.done();
  });

  zcoroutine::go([&reader, &done]() {
    char buffer[8] = {0};
    EXPECT_EQ(reader.recv(buffer, 4, 0, 200), 4);
    EXPECT_STREQ(buffer, "ping");
    done.done();
  });

  done.wait();
}

TEST_F(SocketCoroutineUnitTest, RecvRespectsExplicitTimeoutInCoroutineContext) {
  zcoroutine::init(1);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  Socket reader(pair[0]);
  zcoroutine::WaitGroup done(1);

  std::atomic<ssize_t> result{-2};
  std::atomic<int> err{0};
  std::atomic<int64_t> elapsed_ms{0};

  zcoroutine::go([&]() {
    char buffer[8] = {0};
    const auto started = std::chrono::steady_clock::now();
    errno = 0;
    const ssize_t n = reader.recv(buffer, sizeof(buffer), 0, 30);
    const auto ended = std::chrono::steady_clock::now();

    result.store(n, std::memory_order_release);
    err.store(errno, std::memory_order_release);
    elapsed_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(ended - started)
            .count(),
        std::memory_order_release);
    done.done();
  });

  done.wait();

  EXPECT_EQ(result.load(std::memory_order_acquire), -1);
  EXPECT_EQ(err.load(std::memory_order_acquire), ETIMEDOUT);
  EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 20);

  ::close(pair[1]);
}

TEST_F(SocketCoroutineUnitTest,
       AcceptRespectsExplicitTimeoutInCoroutineContext) {
  zcoroutine::init(1);

  Socket::ptr listener = Socket::create_tcp();
  ASSERT_NE(listener, nullptr);
  ASSERT_TRUE(listener->bind(std::make_shared<IPv4Address>("127.0.0.1", 0)));
  ASSERT_TRUE(listener->listen(8));

  zcoroutine::WaitGroup done(1);
  std::atomic<int> err{0};
  std::atomic<int64_t> elapsed_ms{0};
  std::atomic<bool> timed_out{false};

  zcoroutine::go([&]() {
    const auto started = std::chrono::steady_clock::now();
    errno = 0;
    Socket::ptr peer = listener->accept(30);
    const auto ended = std::chrono::steady_clock::now();

    timed_out.store(peer == nullptr, std::memory_order_release);
    err.store(errno, std::memory_order_release);
    elapsed_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(ended - started)
            .count(),
        std::memory_order_release);
    done.done();
  });

  done.wait();

  EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
  EXPECT_EQ(err.load(std::memory_order_acquire), ETIMEDOUT);
  EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 20);
}

TEST_F(SocketCoroutineUnitTest,
       ConnectRespectsExplicitTimeoutInCoroutineContext) {
  zcoroutine::init(1);

  Socket::ptr client = Socket::create_tcp();
  ASSERT_NE(client, nullptr);

  // TEST-NET-3 通常不可达，适合稳定触发 connect 超时路径。
  Address::ptr target = std::make_shared<IPv4Address>("203.0.113.1", 65000);

  zcoroutine::WaitGroup done(1);
  std::atomic<bool> connected{true};
  std::atomic<int> err{0};
  std::atomic<int64_t> elapsed_ms{0};

  zcoroutine::go([&]() {
    const auto started = std::chrono::steady_clock::now();
    errno = 0;
    const bool ok = client->connect(target, 30);
    const auto ended = std::chrono::steady_clock::now();

    connected.store(ok, std::memory_order_release);
    err.store(errno, std::memory_order_release);
    elapsed_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(ended - started)
            .count(),
        std::memory_order_release);
    done.done();
  });

  done.wait();

  EXPECT_FALSE(connected.load(std::memory_order_acquire));
  EXPECT_EQ(err.load(std::memory_order_acquire), ETIMEDOUT);
  EXPECT_GE(elapsed_ms.load(std::memory_order_acquire), 20);
}

}  // namespace
}  // namespace znet
