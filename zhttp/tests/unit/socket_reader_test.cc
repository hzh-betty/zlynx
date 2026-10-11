#include "../support/server_fixture.h"
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using namespace zhttp::test;
using namespace std::chrono_literals;

namespace {
class SocketReaderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX,
                               SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                               fds),
                  0);
        try {
            peer = zco::io::Descriptor(fds[1]);
        } catch (...) {
            ::close(fds[0]);
            throw;
        }
        client = zco::io::Descriptor(fds[0]);
    }

    zco::io::Descriptor peer, client;
};

TEST_F(SocketReaderTest, PreservesFramesCoalescedWithHandshakeHeaders) {
    const std::string headers =
        "HTTP/1.1 101 Switching Protocols\r\nX: value\r\n\r\n";
    const std::string pong("\x8a\x01p", 3);
    const std::string echo("\x81\x05hello", 7);
    ASSERT_TRUE(send_all(peer.native_handle(), headers + pong + echo));
    SocketReader reader(client.native_handle());
    const auto deadline = zco::Deadline::after(1s);
    EXPECT_EQ(reader.until("\r\n\r\n", deadline).value(), headers);
    EXPECT_EQ(recv_websocket_frame(reader, deadline).value(), pong);
    EXPECT_EQ(recv_websocket_frame(reader, deadline).value(), echo);
}

TEST_F(SocketReaderTest, CollectsBytewiseHeadersAndFrame) {
    const std::string headers = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    const std::string frame("\x81\x05hello", 7);
    ASSERT_TRUE(send_all(peer.native_handle(), headers + frame));
    SocketReader reader(client.native_handle(), 1);
    const auto deadline = zco::Deadline::after(1s);
    EXPECT_EQ(reader.until("\r\n\r\n", deadline).value(), headers);
    EXPECT_EQ(recv_websocket_frame(reader, deadline).value(), frame);
}

TEST_F(SocketReaderTest, PrematureCloseRejectsIncompleteHeaders) {
    ASSERT_TRUE(send_all(peer.native_handle(), "HTTP/1.1 200 OK\r\n"));
    ASSERT_EQ(::shutdown(peer.native_handle(), SHUT_WR), 0);
    SocketReader reader(client.native_handle(), 1);
    auto result = reader.until("\r\n\r\n", zco::Deadline::after(1s));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::connection_reset);
}

TEST_F(SocketReaderTest, PrematureCloseRejectsIncompleteFramePayload) {
    ASSERT_TRUE(send_all(peer.native_handle(), std::string("\x81\x05he", 4)));
    ASSERT_EQ(::shutdown(peer.native_handle(), SHUT_WR), 0);
    SocketReader reader(client.native_handle(), 1);
    auto result = recv_websocket_frame(reader, zco::Deadline::after(1s));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::connection_reset);
}

TEST_F(SocketReaderTest, ReceivesWholeResponseBeforeEof) {
    const std::string response =
        "HTTP/1.1 200 OK\r\n\r\n" + std::string(8192, 'x');
    ASSERT_TRUE(send_all(peer.native_handle(), response));
    ASSERT_EQ(::shutdown(peer.native_handle(), SHUT_WR), 0);
    SocketReader reader(client.native_handle(), 13);
    EXPECT_EQ(reader.until_closed(zco::Deadline::after(1s)).value(), response);
}

TEST_F(SocketReaderTest, TimeoutDoesNotAcceptPartialResponse) {
    ASSERT_TRUE(
        send_all(peer.native_handle(), "HTTP/1.1 200 OK\r\n\r\npartial"));
    SocketReader reader(client.native_handle());
    auto result = reader.until_closed(zco::Deadline::after(30ms));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::timed_out);
}

TEST_F(SocketReaderTest, IncomingFragmentsDoNotRenewReceiveDeadline) {
    auto sender = std::async(std::launch::async, [&] {
        bool sent = true;
        for (int i = 0; i < 5; ++i) {
            sent = send_all(peer.native_handle(), "x") && sent;
            std::this_thread::sleep_for(20ms);
        }
        return ::shutdown(peer.native_handle(), SHUT_WR) == 0 && sent;
    });
    SocketReader reader(client.native_handle(), 1);
    auto result = reader.until_closed(zco::Deadline::after(50ms));
    EXPECT_TRUE(sender.get());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::timed_out);
}

TEST_F(SocketReaderTest, FrameLengthAndPayloadShareAnExpiredDeadline) {
    ASSERT_TRUE(
        send_all(peer.native_handle(), std::string("\x81\x7e\x00\x7e", 4)));
    SocketReader reader(client.native_handle(), 1);
    const auto deadline = zco::Deadline::after(30ms);
    auto result = recv_websocket_frame(reader, deadline);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::timed_out);
    auto next = reader.exact(1, deadline);
    ASSERT_FALSE(next);
    EXPECT_EQ(next.error(), std::errc::timed_out);
}

TEST_F(SocketReaderTest, InvalidDescriptorFailsWithoutWaitingForTimeout) {
    SocketReader reader(-1);
    auto result = reader.exact(1, zco::Deadline::after(30ms));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::errc::bad_file_descriptor);
}
} // namespace
