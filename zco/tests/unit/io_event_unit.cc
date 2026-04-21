#include <atomic>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/io_event.h"

namespace zco {
namespace {

class IoEventUnitTest : public test::RuntimeTestBase {};

TEST_F(IoEventUnitTest, InvalidFdReturnsFalseAndEbadf) {
    IoEvent event(-1, IoEventType::kRead);

    errno = 0;
    EXPECT_FALSE(event.wait(1));
    EXPECT_EQ(errno, EBADF);
}

TEST_F(IoEventUnitTest, InvalidEventTypeReturnsFalseAndEinval) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    IoEvent event(pair[0], static_cast<IoEventType>(0x999));
    errno = 0;
    EXPECT_FALSE(event.wait(1));
    EXPECT_EQ(errno, EINVAL);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, ReadReadyReturnsTrue) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    go([&done, fd_read = pair[1], fd_write = pair[0]]() {
        const char marker = 'z';
        ASSERT_EQ(::write(fd_write, &marker, 1), 1);

        IoEvent event(fd_read, IoEventType::kRead);
        EXPECT_TRUE(event.wait(20));

        char out = 0;
        EXPECT_EQ(::read(fd_read, &out, 1), 1);
        EXPECT_EQ(out, marker);
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, ReadTimeoutReturnsFalse) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    go([&done, fd = pair[1]]() {
        IoEvent event(fd, IoEventType::kRead);
        errno = 0;
        EXPECT_FALSE(event.wait(5));
        EXPECT_TRUE(errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK);
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, WriteReadyReturnsTrue) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        IoEvent event(fd, IoEventType::kWrite);
        EXPECT_TRUE(event.wait(10));
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, OutsideCoroutineWaitReturnsEperm) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    IoEvent event(pair[1], IoEventType::kRead);
    errno = 0;
    EXPECT_FALSE(event.wait(5));
    EXPECT_EQ(errno, EPERM);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, CoroutineTimeoutKeepsTimeoutStateAndPropagatesErrno) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    std::atomic<bool> timed_out(false);
    std::atomic<int> captured_errno(0);

    go([&done, &timed_out, &captured_errno, fd = pair[1]]() {
        IoEvent event(fd, IoEventType::kRead);
        errno = 0;
        const bool ok = event.wait(8);
        timed_out.store(!ok && timeout(), std::memory_order_release);
        captured_errno.store(errno, std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_TRUE(timed_out.load(std::memory_order_acquire));
    const int err = captured_errno.load(std::memory_order_acquire);
    EXPECT_TRUE(err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK);

    ::close(pair[0]);
    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, CoroutineWaitOnClosedFdFailsWithEbadf) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ASSERT_EQ(::close(pair[0]), 0);

    WaitGroup done(1);
    go([&done, fd = pair[0]]() {
        IoEvent event(fd, IoEventType::kRead);
        errno = 0;
        EXPECT_FALSE(event.wait(10));
        EXPECT_TRUE(errno == EINVAL || errno == EBADF);
        done.done();
    });
    done.wait();

    ::close(pair[1]);
}

TEST_F(IoEventUnitTest, CoroutineZeroTimeoutWithoutReadyEventReturnsEtimedout) {
    init(1);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    WaitGroup done(1);
    go([&done, fd = pair[1]]() {
        IoEvent event(fd, IoEventType::kRead);
        errno = 0;
        EXPECT_FALSE(event.wait(0));
        EXPECT_EQ(errno, ETIMEDOUT);
        done.done();
    });
    done.wait();

    ::close(pair[0]);
    ::close(pair[1]);
}

} // namespace
} // namespace zco
