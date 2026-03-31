#include "znet/session.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"
#include "znet/tcp_connection.h"

namespace znet {
namespace {

class StreamUnitTest : public ::testing::Test {
 protected:
  void TearDown() override { zcoroutine::shutdown(); }
};

class PassiveStream : public Stream {
 protected:
  ssize_t do_read(void* /*buffer*/, size_t /*length*/,
                  uint32_t /*timeout_ms*/) override {
    errno = EAGAIN;
    return -1;
  }

  ssize_t do_write(const void* /*buffer*/, size_t length,
                   uint32_t /*timeout_ms*/) override {
    return static_cast<ssize_t>(length);
  }

  size_t pending_bytes() const override { return 0; }
};

TEST_F(StreamUnitTest, StreamDefaultBufferHooksAreNoop) {
  auto stream = std::make_shared<PassiveStream>();
  EXPECT_EQ(stream->read_to_buffer(128), 0);
  EXPECT_EQ(stream->flush_buffer(), 0);
}

TEST_F(StreamUnitTest, SocketStreamReadsAndWritesWithoutInternalBuffer) {
  zcoroutine::init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<SocketStream>();
  auto write_stream = std::make_shared<SocketStream>();
  conn->set_streams(read_stream, write_stream);

  zcoroutine::WaitGroup done(1);
  zcoroutine::go([conn, &done]() {
    conn->bind_to_current_loop();
    EXPECT_EQ(conn->read(1024), 4);

    char in[8] = {0};
    EXPECT_EQ(conn->read_stream()->read(in, 4), 4);
    EXPECT_STREQ(in, "ping");

    EXPECT_EQ(conn->send("pong", 4), 4);
    done.done();
  });

  ASSERT_EQ(::send(pair[1], "ping", 4, 0), 4);
  done.wait();

  char out[8] = {0};
  ASSERT_EQ(::recv(pair[1], out, 4, 0), 4);
  EXPECT_STREQ(out, "pong");

  ::close(pair[1]);
}

TEST_F(StreamUnitTest, BufferStreamReadsFromConnectionInputBuffer) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto connection = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  connection->set_streams(read_stream, write_stream);
  ASSERT_EQ(read_stream->write("hello", 5), 5);

  char out[8] = {0};
  ASSERT_EQ(read_stream->read(out, 5), 5);
  EXPECT_STREQ(out, "hello");

  errno = 0;
  EXPECT_EQ(read_stream->read(out, 1), -1);
  EXPECT_EQ(errno, EAGAIN);

  ::close(pair[1]);
}

TEST_F(StreamUnitTest, BufferStreamWritesToConnectionOutputBuffer) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto connection = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  connection->set_streams(read_stream, write_stream);

  ASSERT_EQ(write_stream->write("pong", 4), 4);
  char out[8] = {0};
  ASSERT_EQ(write_stream->read(out, 4), 4);
  EXPECT_STREQ(out, "pong");

  ::close(pair[1]);
}

TEST_F(StreamUnitTest, BufferStreamReadWithoutDataReturnsEagain) {
  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto connection = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto read_stream = std::make_shared<BufferStream>();
  auto write_stream = std::make_shared<BufferStream>();
  connection->set_streams(read_stream, write_stream);

  char out[4] = {0};
  errno = 0;
  EXPECT_EQ(read_stream->read(out, sizeof(out)), -1);
  EXPECT_EQ(errno, EAGAIN);

  ::close(pair[1]);
}

TEST_F(StreamUnitTest, BufferStreamWorksInCoroutineReadWriteFlow) {
  zcoroutine::init(2);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  auto writer_conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[0]));
  auto reader_conn = std::make_shared<TcpConnection>(std::make_shared<Socket>(pair[1]));

  auto writer_stream = std::make_shared<BufferStream>();
  auto reader_stream = std::make_shared<BufferStream>();
  auto writer_out = std::make_shared<BufferStream>();
  auto reader_out = std::make_shared<BufferStream>();
  writer_conn->set_streams(writer_stream, writer_out);
  reader_conn->set_streams(reader_stream, reader_out);

  zcoroutine::WaitGroup done(2);

  zcoroutine::go([writer_conn, writer_out, &done]() {
    writer_conn->bind_to_current_loop();
    EXPECT_EQ(writer_out->write("ping", 4), 4);
    EXPECT_EQ(writer_conn->flush_output(), 4);
    done.done();
  });

  zcoroutine::go([reader_conn, reader_stream, &done]() {
    reader_conn->bind_to_current_loop();
    EXPECT_EQ(reader_conn->read(1024), 4);
    char out[8] = {0};
    EXPECT_EQ(reader_stream->read(out, 4), 4);
    EXPECT_STREQ(out, "ping");
    done.done();
  });

  done.wait();
}

}  // namespace
}  // namespace znet
