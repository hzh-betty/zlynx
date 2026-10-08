#include "support/runtime_fixture.h"
#include <sys/socket.h>
#include <unistd.h>
using namespace zco;

TEST(SocketIO, ExactReadPreservesPartialProgressAtEofAndTimeout) {
    Runtime runtime(RuntimeOptions{1});
    for (bool eof : {false, true}) {
        int pair[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair),
                  0);
        io::Descriptor descriptor(pair[0]), peer(pair[1]);
        ASSERT_EQ(::write(peer.native_handle(), "abc", 3), 3);
        if (eof)
            ASSERT_EQ(::shutdown(peer.native_handle(), SHUT_WR), 0);
        auto task = test::spawn(runtime, [&] {
            char data[8];
            auto result = io::read_exact(descriptor, data, 8, test::ms(10));
            EXPECT_EQ(result.bytes, 3u);
            EXPECT_EQ(std::string(data, result.bytes), "abc");
            EXPECT_EQ(result.eof, eof);
            EXPECT_EQ(result.error, eof ? std::error_code{}
                                        : wait_error(WaitOutcome::timeout));
        });
        EXPECT_TRUE(task.join(test::soon()));
    }
}

TEST(SocketIO, WriteAllPreservesPartialProgressOnTimeout) {
    Runtime runtime(RuntimeOptions{1});
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    int size = 4096;
    ASSERT_EQ(::setsockopt(descriptor.native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &size, sizeof(size)),
              0);
    auto task = test::spawn(runtime, [&] {
        std::string data(1024 * 1024, 'a');
        auto result =
            io::write_all(descriptor, data.data(), data.size(), test::ms(10));
        EXPECT_GT(result.bytes, 0u);
        EXPECT_LT(result.bytes, data.size());
        EXPECT_EQ(result.error, wait_error(WaitOutcome::timeout));
    });
    EXPECT_TRUE(task.join(test::soon()));
}

TEST(SocketIO, KernelSocketTimeoutIsQueriedPerOperation) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    EXPECT_TRUE(io::socket_deadline(descriptor, true).value().is_infinite());
    timeval timeout{0, 10000};
    ASSERT_EQ(::setsockopt(descriptor.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                           &timeout, sizeof(timeout)),
              0);
    EXPECT_FALSE(io::socket_deadline(descriptor, true).value().is_infinite());
    timeout = {0, 0};
    ASSERT_EQ(::setsockopt(descriptor.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                           &timeout, sizeof(timeout)),
              0);
    EXPECT_TRUE(io::socket_deadline(descriptor, true).value().is_infinite());
}
