#define private public
#include "znet/acceptor.h"
#undef private

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "zco/sched.h"
#include "zco/wait_group.h"

namespace znet {
namespace {

class AcceptorUnitTest : public ::testing::Test {
  protected:
    void TearDown() override { zco::shutdown(); }
};

int connect_loopback(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

TEST_F(AcceptorUnitTest, StartFailsWhenListenAddressIsNull) {
    auto acceptor = std::make_shared<Acceptor>(Address::ptr{}, 8);
    ASSERT_NE(acceptor, nullptr);

    EXPECT_FALSE(acceptor->start());
    EXPECT_FALSE(acceptor->is_running());
}

TEST_F(AcceptorUnitTest, StartOnStackInstanceFailsBadWeakPtr) {
    Acceptor acceptor(std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    EXPECT_FALSE(acceptor.start());
    EXPECT_FALSE(acceptor.is_running());
}

TEST_F(AcceptorUnitTest, StartFailsWhenBindOrListenFails) {
    // AF_UNIX 地址走 TCP 创建路径会导致 bind/listen 失败，覆盖失败回滚分支。
    auto acceptor =
        std::make_shared<Acceptor>(std::make_shared<UnixAddress>(""), 8);
    ASSERT_NE(acceptor, nullptr);

    EXPECT_FALSE(acceptor->start());
    EXPECT_FALSE(acceptor->is_running());
}

TEST_F(AcceptorUnitTest, StartStopAreIdempotent) {
    zco::init(1);

    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(acceptor->start());
    ASSERT_TRUE(acceptor->start());
    ASSERT_TRUE(acceptor->is_running());
    ASSERT_NE(acceptor->listen_socket(), nullptr);

    acceptor->stop();
    acceptor->stop();
    EXPECT_FALSE(acceptor->is_running());
}

TEST_F(AcceptorUnitTest, AcceptCallbackReceivesClientSocket) {
    zco::init(2);

    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);

    std::atomic<int> accepted_count{0};
    std::atomic<int> client_fd{-1};
    acceptor->set_accept_callback([&](Socket::ptr client) {
        ASSERT_NE(client, nullptr);
        client_fd.store(client->fd(), std::memory_order_release);
        accepted_count.fetch_add(1, std::memory_order_release);
        client->close();
    });

    ASSERT_TRUE(acceptor->start());
    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        acceptor->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int fd = connect_loopback(bound_addr->port());
    ASSERT_GE(fd, 0);
    ::close(fd);

    for (int i = 0;
         i < 100 && accepted_count.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(accepted_count.load(std::memory_order_acquire), 1);
    EXPECT_GE(client_fd.load(std::memory_order_acquire), 0);

    acceptor->stop();
}

TEST_F(AcceptorUnitTest, NullCallbackPathDoesNotBlockStop) {
    zco::init(1);

    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);
    ASSERT_TRUE(acceptor->start());

    auto bound_addr = std::dynamic_pointer_cast<IPv4Address>(
        acceptor->listen_socket()->get_local_address());
    ASSERT_NE(bound_addr, nullptr);

    int fd = connect_loopback(bound_addr->port());
    ASSERT_GE(fd, 0);
    ::close(fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    acceptor->stop();
    EXPECT_FALSE(acceptor->is_running());
}

TEST_F(AcceptorUnitTest, AcceptLoopExitsWhenListenSocketBecomesNull) {
    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);

    acceptor->running_.store(true, std::memory_order_release);
    acceptor->listen_socket_.reset();
    acceptor->accept_loop();
}

TEST_F(AcceptorUnitTest, AcceptLoopHandlesGeneralAcceptErrorAndBackoff) {
    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);

    acceptor->listen_socket_ = Socket::create_tcp();
    ASSERT_NE(acceptor->listen_socket_, nullptr);
    acceptor->running_.store(true, std::memory_order_release);

    std::thread worker([&]() { acceptor->accept_loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    acceptor->running_.store(false, std::memory_order_release);
    worker.join();
}

TEST_F(AcceptorUnitTest, AcceptLoopBreaksOnEbafdAfterAcceptFailure) {
    zco::init(1);

    auto acceptor = std::make_shared<Acceptor>(
        std::make_shared<IPv4Address>("127.0.0.1", 0), 8);
    ASSERT_NE(acceptor, nullptr);
    acceptor->listen_socket_ = Socket::create_tcp();
    ASSERT_NE(acceptor->listen_socket_, nullptr);
    acceptor->listen_socket_->close();
    acceptor->running_.store(true, std::memory_order_release);

    zco::WaitGroup done(1);
    zco::go([&]() {
        errno = EBADF;
        acceptor->accept_loop();
        done.done();
    });
    done.wait();
}

} // namespace
} // namespace znet
