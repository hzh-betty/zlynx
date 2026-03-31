#include "znet/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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
  EXPECT_EQ(errno, EOPNOTSUPP);

  ::close(pair[1]);
}

TEST_F(SocketCoroutineUnitTest, ConnectOutsideCoroutineFailsFast) {
  Socket::ptr client = Socket::create_tcp();
  ASSERT_NE(client, nullptr);

  Address::ptr target = std::make_shared<IPv4Address>("127.0.0.1", 9);
  errno = 0;
  EXPECT_FALSE(client->connect(target, 10));
  EXPECT_EQ(errno, EOPNOTSUPP);
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
    EXPECT_EQ(writer.send(payload, sizeof(payload) - 1),
              static_cast<ssize_t>(sizeof(payload) - 1));
    done.done();
  });

  zcoroutine::go([&reader, &done]() {
    char buffer[8] = {0};
    EXPECT_EQ(reader.recv(buffer, 4), 4);
    EXPECT_STREQ(buffer, "ping");
    done.done();
  });

  done.wait();
}

}  // namespace
}  // namespace znet
