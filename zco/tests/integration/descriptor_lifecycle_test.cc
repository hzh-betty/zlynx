#include "support/worker_fixture.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace zco;

TEST(Descriptor, MoveCloseAndDuplicateHaveUniqueResourceIdentity) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor first(pair[0]), second(pair[1]);
    auto copy = first.duplicate();
    ASSERT_TRUE(copy);
    EXPECT_NE(copy.value().id().value, first.id().value);
    auto id = first.id().value;
    io::Descriptor moved(std::move(first));
    EXPECT_EQ(first.native_handle(), -1);
    EXPECT_EQ(moved.id().value, id);
    EXPECT_TRUE(moved.close());
    EXPECT_TRUE(moved.close());
    EXPECT_GE(copy.value().native_handle(), 0);
}

TEST(Descriptor, CloseRevokesWaitAndReusedFdGetsANewIdentity) {
    Runtime runtime(RuntimeOptions{1});
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    Event entered(true);
    auto task = test::spawn(runtime, [&] {
        entered.signal();
        char data;
        auto result = io::read_some(descriptor, &data, 1);
        EXPECT_EQ(result.error, wait_error(WaitOutcome::closed));
    });
    ASSERT_TRUE(entered.wait(test::soon()));
    auto old = descriptor.id().value;
    int fd = descriptor.native_handle();
    EXPECT_TRUE(descriptor.close());
    EXPECT_TRUE(task.join(test::soon()));
    int next[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, next), 0);
    io::Descriptor fresh(next[0]), fresh_peer(next[1]);
    EXPECT_NE(fresh.id().value, old);
    EXPECT_EQ(fresh.native_handle(), fd);
    auto read = test::spawn(runtime, [&] {
        char data;
        EXPECT_TRUE(io::read_some(fresh, &data, 1, test::soon()));
        EXPECT_EQ(data, 'x');
    });
    ASSERT_EQ(::write(fresh_peer.native_handle(), "x", 1), 1);
    EXPECT_TRUE(read.join(test::soon()));
}

TEST(Descriptor, ReplacementCancelsOldWaitAndStaleEventsCannotWakeNewWait) {
    test::WorkerFixture fixture;
    int first[2], second[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, first), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, second), 0);
    io::Descriptor target(first[0]), old_peer(first[1]), source(second[0]),
        new_peer(second[1]);
    auto old_task = fixture.spawn([&] {
        EXPECT_EQ(io::wait_ready(target, io::Interest::read).error(),
                  wait_error(WaitOutcome::closed));
    });
    auto stale = fixture.reactor->await_registration();
    auto old_id = target.id().value;
    ASSERT_TRUE(target.replace_with_duplicate(source));
    EXPECT_NE(target.id().value, old_id);
    EXPECT_TRUE(old_task.join(test::soon()));
    auto next_task = fixture.spawn(
        [&] { EXPECT_TRUE(io::wait_ready(target, io::Interest::read)); });
    auto fresh = fixture.reactor->await_registration();
    ASSERT_NE(stale, fresh);
    fixture.reactor->emit(stale, io::Interest::read);
    // Pinned barrier executes after the stale event is consumed. A spurious
    // wake would have finished next_task before this task runs.
    EXPECT_TRUE(
        fixture
            .spawn([&] { EXPECT_EQ(next_task.status(), TaskStatus::running); })
            .join(test::soon()));
    fixture.reactor->emit(fresh, io::Interest::read);
    EXPECT_TRUE(next_task.join(test::soon()));
}
