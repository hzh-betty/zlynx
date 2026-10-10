#include "support/runtime_fixture.h"
#include <cstdarg>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace zco;

namespace {
std::atomic<int> queries{0};
std::atomic<bool> fail_query{false};
} // namespace

extern "C" int __real_fcntl(int, int, ...);
extern "C" int __wrap_fcntl(int fd, int command, ...) {
    if (command == F_GETFL) {
        ++queries;
        if (fail_query.exchange(false)) {
            errno = EIO;
            return -1;
        }
    }
    if (command == F_SETFL || command == F_DUPFD_CLOEXEC || command == F_DUPFD) {
        va_list arguments;
        va_start(arguments, command);
        int argument = va_arg(arguments, int);
        va_end(arguments);
        return __real_fcntl(fd, command, argument);
    }
    return __real_fcntl(fd, command);
}

TEST(DescriptorFlags, AdoptionChecksOnceAndTransfersDoNotRequery) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    queries = 0;
    io::Descriptor reader(pair[0]), writer(pair[1]);
    EXPECT_EQ(queries, 2);
    for (int i = 0; i < 256; ++i) {
        char sent = static_cast<char>(i), received = 0;
        auto wrote = io::write_all(writer, &sent, 1);
        auto read = io::read_exact(reader, &received, 1);
        EXPECT_FALSE(wrote.error);
        EXPECT_FALSE(read.error);
        EXPECT_EQ(wrote.bytes, 1u);
        EXPECT_EQ(read.bytes, 1u);
        EXPECT_EQ(received, sent);
    }
    EXPECT_EQ(queries, 2);
    auto duplicate = reader.duplicate();
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(queries, 3);
    EXPECT_TRUE(reader.close());
    char sent = 'x', received = 0;
    EXPECT_TRUE(io::write_some(writer, &sent, 1));
    EXPECT_TRUE(io::read_some(duplicate.value(), &received, 1));
    EXPECT_EQ(received, sent);
    EXPECT_EQ(queries, 3);
}

TEST(DescriptorFlags, BlockingStateIsRejectedWithoutAnotherQuery) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    queries = 0;
    io::Descriptor reader(pair[0]), writer(pair[1]);
    char data = 0;
    EXPECT_EQ(io::read_some(reader, &data, 1).error,
              std::make_error_code(std::errc::operation_not_permitted));
    EXPECT_EQ(io::write_some(writer, &data, 1).error,
              std::make_error_code(std::errc::operation_not_permitted));
    EXPECT_EQ(queries, 2);
}

TEST(DescriptorFlags, FailedAdoptionQueryClosesTheOwnedFd) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor peer(pair[1]);
    fail_query = true;
    EXPECT_THROW(io::Descriptor descriptor(pair[0]), std::system_error);
    EXPECT_EQ(__real_fcntl(pair[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(DescriptorFlags, NonSocketTransferStillUsesTheNonblockingContract) {
    int pair[2];
    ASSERT_EQ(::pipe2(pair, O_NONBLOCK | O_CLOEXEC), 0);
    queries = 0;
    io::Descriptor reader(pair[0]), writer(pair[1]);
    char sent = 'p', received = 0;
    EXPECT_TRUE(io::write_all(writer, &sent, 1));
    EXPECT_TRUE(io::read_exact(reader, &received, 1));
    EXPECT_EQ(received, sent);
    EXPECT_EQ(queries, 2);
}
