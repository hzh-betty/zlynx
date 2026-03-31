#include "znet/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

#include <gtest/gtest.h>

#include "zcoroutine/sched.h"

namespace znet {
namespace {

class TaggedStream : public BufferStream {
 public:
  explicit TaggedStream(int tag) : tag_(tag) {}

  int tag() const { return tag_; }

 private:
  int tag_;
};

class TcpServerUnitTest : public ::testing::Test {
 protected:
  void TearDown() override { zcoroutine::shutdown(); }
};

TEST_F(TcpServerUnitTest, AcceptsConnectionAndCreatesSession) {
  zcoroutine::init(2);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

  std::atomic<int> message_events{0};
  std::atomic<int> total_bytes{0};
  server->set_on_message([&](const TcpConnection::ptr& conn, Stream::ptr stream) {
    ASSERT_NE(conn, nullptr);
    ASSERT_NE(stream, nullptr);
    char chunk[32] = {0};
    ssize_t n = 0;
    while ((n = stream->read(chunk, sizeof(chunk))) > 0) {
      total_bytes.fetch_add(static_cast<int>(n));
    }
    message_events.fetch_add(1);
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
  ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
            0);
  ASSERT_EQ(::send(client_fd, "ping", 4, 0), 4);
  ::close(client_fd);

  for (int i = 0; i < 50 && message_events.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GE(message_events.load(), 1);
  EXPECT_EQ(total_bytes.load(), 4);

  server->stop();
}

TEST_F(TcpServerUnitTest, OnMessageCallbackUsesConnectionAndSession) {
  zcoroutine::init(3);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

  std::atomic<int> callback_count{0};
  std::atomic<int> total_bytes{0};
  std::unordered_map<int, int> connection_sched;

  server->set_on_message([&](const TcpConnection::ptr& conn, Stream::ptr stream) {
    ASSERT_NE(conn, nullptr);
    ASSERT_NE(stream, nullptr);
    const int fd = conn->fd();
    const int sched = conn->owner_sched_id();
    ASSERT_GE(sched, 0);

    auto it = connection_sched.find(fd);
    if (it == connection_sched.end()) {
      connection_sched.emplace(fd, sched);
    } else {
      EXPECT_EQ(it->second, sched);
    }

    char chunk[32] = {0};
    ssize_t n = 0;
    while ((n = stream->read(chunk, sizeof(chunk))) > 0) {
      total_bytes.fetch_add(static_cast<int>(n));
    }

    callback_count.fetch_add(1);
  });

  ASSERT_TRUE(server->start());
  auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
      server->acceptor()->listen_socket()->get_local_address());
  ASSERT_NE(bound_addr, nullptr);

  auto send_payload = [&](const char* msg) {
    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bound_addr->port());
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(
        ::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
        0);
    ASSERT_EQ(::send(client_fd, msg, 4, 0), 4);
    ASSERT_EQ(::send(client_fd, msg, 4, 0), 4);
    ::close(client_fd);
  };

  send_payload("msg1");

  for (int i = 0; i < 100 && callback_count.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GE(callback_count.load(), 1);
  EXPECT_EQ(total_bytes.load(), 8);
  EXPECT_EQ(connection_sched.size(), 1U);

  server->stop();
}

TEST_F(TcpServerUnitTest, ConnectAndCloseCallbacksAreIndependent) {
  zcoroutine::init(2);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

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
      ::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
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

TEST_F(TcpServerUnitTest, SnakeCaseMessageCallbackReceivesSession) {
  zcoroutine::init(2);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

  std::atomic<int> connection_events{0};
  std::atomic<int> message_events{0};
  std::string merged_payload;

  server->set_on_connection([&](const std::shared_ptr<TcpConnection>& conn) {
    EXPECT_NE(conn, nullptr);
    connection_events.fetch_add(1);
  });

  server->set_on_message(
      [&](const TcpConnection::ptr& conn, Stream::ptr stream) {
        EXPECT_NE(conn, nullptr);
        EXPECT_NE(stream, nullptr);
        char chunk[16] = {0};
        ssize_t n = 0;
        while ((n = stream->read(chunk, sizeof(chunk))) > 0) {
          merged_payload.append(chunk, static_cast<size_t>(n));
        }
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
      ::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
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

TEST_F(TcpServerUnitTest, KeepsConnectionAliveUntilPeerCloses) {
  zcoroutine::init(2);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

  std::atomic<int> message_count{0};
  std::atomic<int> close_count{0};

  server->set_on_message([&](const TcpConnection::ptr& conn, Stream::ptr stream) {
    ASSERT_NE(conn, nullptr);
    ASSERT_NE(stream, nullptr);

    char chunk[16] = {0};
    ssize_t n = 0;
    while ((n = stream->read(chunk, sizeof(chunk))) > 0) {
      (void)n;
    }
    message_count.fetch_add(1);
  });

  server->set_on_close([&](const TcpConnection::ptr& conn) {
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
      ::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
      0);

  ASSERT_EQ(::send(client_fd, "a", 1, 0), 1);
  for (int i = 0; i < 100 && message_count.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(message_count.load(), 1);
  EXPECT_EQ(close_count.load(), 0);

  ASSERT_EQ(::send(client_fd, "b", 1, 0), 1);
  for (int i = 0; i < 100 && message_count.load() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(message_count.load(), 2);
  EXPECT_EQ(close_count.load(), 0);

  ::close(client_fd);

  for (int i = 0; i < 100 && close_count.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(close_count.load(), 1);

  server->stop();
}

TEST_F(TcpServerUnitTest, UsesConfiguredStreamFactory) {
  zcoroutine::init(2);

  auto listen_addr = std::make_shared<IPv4Address>("127.0.0.1", 0);
  auto server = std::make_shared<TcpServer>(listen_addr, 16);

  std::atomic<int> message_count{0};
  std::atomic<int> seen_read_tag{-1};
  std::atomic<int> seen_write_tag{-1};

  server->set_stream_factory([]() {
    return std::make_pair(std::make_shared<TaggedStream>(101),
                          std::make_shared<TaggedStream>(202));
  });

  server->set_on_connection([&](const TcpConnection::ptr& conn) {
    ASSERT_NE(conn, nullptr);
    auto tagged_write = std::dynamic_pointer_cast<TaggedStream>(conn->write_stream());
    ASSERT_NE(tagged_write, nullptr);
    seen_write_tag.store(tagged_write->tag());
  });

  server->set_on_message([&](const TcpConnection::ptr& conn, Stream::ptr stream) {
    ASSERT_NE(conn, nullptr);
    auto tagged_read = std::dynamic_pointer_cast<TaggedStream>(stream);
    ASSERT_NE(tagged_read, nullptr);
    seen_read_tag.store(tagged_read->tag());

    char chunk[16] = {0};
    while (stream->read(chunk, sizeof(chunk)) > 0) {
    }
    message_count.fetch_add(1);
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
      ::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
      0);

  ASSERT_EQ(::send(client_fd, "xy", 2, 0), 2);
  ::close(client_fd);

  for (int i = 0; i < 100 && message_count.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GE(message_count.load(), 1);
  EXPECT_EQ(seen_read_tag.load(), 101);
  EXPECT_EQ(seen_write_tag.load(), 202);

  server->stop();
}

}  // namespace
}  // namespace znet
