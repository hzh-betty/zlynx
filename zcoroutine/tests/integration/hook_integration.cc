#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class HookIntegrationTest : public test::RuntimeTestBase {};

TEST_F(HookIntegrationTest, SocketPairReadWriteVectorAndDatagramFlow) {
  init(2);

  int stream_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);

  const char* msg = "hello";
  EXPECT_EQ(co_write(stream_pair[0], msg, 5, 200), 5);

  char recv_buffer[8] = {0};
  EXPECT_EQ(co_read(stream_pair[1], recv_buffer, 5, 200), 5);
  EXPECT_STREQ(recv_buffer, "hello");

  const char* left = "ab";
  const char* right = "cd";
  iovec write_vec[2];
  write_vec[0].iov_base = const_cast<char*>(left);
  write_vec[0].iov_len = 2;
  write_vec[1].iov_base = const_cast<char*>(right);
  write_vec[1].iov_len = 2;
  EXPECT_EQ(co_writev(stream_pair[0], write_vec, 2, 200), 4);

  char out_left[3] = {0};
  char out_right[3] = {0};
  iovec read_vec[2];
  read_vec[0].iov_base = out_left;
  read_vec[0].iov_len = 2;
  read_vec[1].iov_base = out_right;
  read_vec[1].iov_len = 2;
  EXPECT_EQ(co_readv(stream_pair[1], read_vec, 2, 200), 4);
  EXPECT_STREQ(out_left, "ab");
  EXPECT_STREQ(out_right, "cd");

  ::close(stream_pair[0]);
  ::close(stream_pair[1]);

  int dgram_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram_pair), 0);

  const char* datagram = "hook";
  EXPECT_EQ(co_sendto(dgram_pair[0], datagram, 4, 0, nullptr, 0, 200), 4);

  char datagram_out[8] = {0};
  sockaddr_storage recv_addr;
  socklen_t recv_len = sizeof(recv_addr);
  EXPECT_EQ(co_recvfrom(dgram_pair[1], datagram_out, 4, 0,
                        reinterpret_cast<sockaddr*>(&recv_addr), &recv_len, 200),
            4);
  EXPECT_STREQ(datagram_out, "hook");

  ::close(dgram_pair[0]);
  ::close(dgram_pair[1]);
}

TEST_F(HookIntegrationTest, SingleAcceptorLoopHandlesMultipleClients) {
  init(4);

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);

  int enable = 1;
  ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)), 0);

  sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listen_addr.sin_port = 0;

  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 16), 0);

  socklen_t listen_addr_len = sizeof(listen_addr);
  ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), &listen_addr_len),
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
          co_accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len, SOCK_CLOEXEC, 2000);
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
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      ASSERT_GE(fd, 0);
      EXPECT_EQ(co_connect(fd, reinterpret_cast<const sockaddr*>(&listen_addr), sizeof(listen_addr),
                           2000),
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
      ASSERT_EQ(co_write(fd, &n, sizeof(n), 500), static_cast<ssize_t>(sizeof(n)));
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
      ASSERT_EQ(co_read(fd, &n, sizeof(n), 500), static_cast<ssize_t>(sizeof(n)));
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
    ASSERT_EQ(::bind(probe_fd, reinterpret_cast<sockaddr*>(&probe_addr), sizeof(probe_addr)), 0);

    socklen_t probe_len = sizeof(probe_addr);
    ASSERT_EQ(::getsockname(probe_fd, reinterpret_cast<sockaddr*>(&probe_addr), &probe_len), 0);
    const uint16_t closed_port = ntohs(probe_addr.sin_port);
    ::close(probe_fd);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;
    target_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target_addr.sin_port = htons(static_cast<uint16_t>(closed_port + (rng() % 3)));

    errno = 0;
    const int rc = co_connect(fd, reinterpret_cast<const sockaddr*>(&target_addr),
                              sizeof(target_addr), 120);
    EXPECT_EQ(rc, -1);
    EXPECT_TRUE(errno == ECONNREFUSED || errno == ETIMEDOUT || errno == EHOSTUNREACH ||
                errno == ENETUNREACH || errno == EINVAL || errno == EADDRNOTAVAIL);

    ::close(fd);
  }
}

TEST_F(HookIntegrationTest, MultiRoundRandomizedAcceptConnectDeterministicSeed) {
  init(4);

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);

  int enable = 1;
  ASSERT_EQ(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)), 0);

  sockaddr_in listen_addr;
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listen_addr.sin_port = 0;

  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 32), 0);

  socklen_t listen_addr_len = sizeof(listen_addr);
  ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&listen_addr), &listen_addr_len),
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
        const int fd =
            co_accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len, SOCK_CLOEXEC, 2000);
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
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        EXPECT_EQ(co_connect(fd, reinterpret_cast<const sockaddr*>(&listen_addr), sizeof(listen_addr),
                             2000),
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

TEST_F(HookIntegrationTest, IndependentStackSocketPairRoundTripWithYieldKeepsStackLocalData) {
  co_stack_model(StackModel::kIndependent);
  co_stack_size(64 * 1024);
  co_stack_num(1);
  init(2);

  int stream_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair), 0);

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

      EXPECT_EQ(co_write(fd, payload, sizeof(payload), 500), static_cast<ssize_t>(sizeof(payload)));

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
      EXPECT_EQ(co_read(fd, payload, sizeof(payload), 500), static_cast<ssize_t>(sizeof(payload)));

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

}  // namespace
}  // namespace zcoroutine
