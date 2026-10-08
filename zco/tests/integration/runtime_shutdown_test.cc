#include "support/runtime_fixture.h"
#include <sys/socket.h>
#include <unistd.h>
using namespace zco;

TEST(Shutdown, AllInfiniteWaitsUnwindAndReleaseExecutionAndCaptures) {
    Event event;
    Mutex mutex;
    ASSERT_TRUE(mutex.lock());
    WaitGroup group(1);
    Channel<int> receiving(1), sending(1);
    ASSERT_TRUE(sending.send(1));
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    std::vector<TaskHandle> tasks;
    std::vector<std::weak_ptr<int>> captures;
    std::atomic<int> returned{0};
    {
        Runtime runtime(RuntimeOptions{2});
        Event entered(true);
        std::atomic<int> started{0};
        std::vector<std::function<Result<void>()>> waits{
            [&] { return event.wait(); },
            [&] { return mutex.lock(); },
            [&] { return group.wait(); },
            [&] { return sending.send(2); },
            [&] {
                auto value = receiving.receive();
                return Result<void>(value.error());
            },
            [&] { return io::wait_ready(descriptor, io::Interest::read); },
            [&] { return sleep_until(Deadline::infinite()); }};
        for (auto &wait : waits) {
            auto capture = std::make_shared<int>(1);
            captures.push_back(capture);
            tasks.push_back(test::spawn(runtime, [&, capture, wait] {
                if (++started == 7)
                    entered.signal();
                auto result = wait();
                EXPECT_EQ(result.error(), wait_error(WaitOutcome::canceled));
                ++returned;
            }));
        }
        ASSERT_TRUE(entered.wait(test::soon()));
        runtime.request_stop();
        runtime.join();
    }
    EXPECT_EQ(returned, 7);
    for (auto &capture : captures)
        EXPECT_TRUE(capture.expired());
    for (auto &task : tasks)
        EXPECT_TRUE(task.join(test::soon()));
    mutex.unlock();
}

TEST(Shutdown, TasksNotYetExecutedAreCanceledAndReleaseCaptures) {
    Runtime runtime(RuntimeOptions{1});
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto running = test::spawn(runtime.executor(0), [&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    auto pending = test::spawn(
        runtime.executor(0), [capture] { FAIL() << "Canceled task executed"; });
    capture.reset();
    runtime.request_stop();
    release.set_value();
    runtime.join();
    EXPECT_EQ(pending.status(), TaskStatus::canceled);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(pending.join().error(), wait_error(WaitOutcome::canceled));
    EXPECT_TRUE(running.join());
}
