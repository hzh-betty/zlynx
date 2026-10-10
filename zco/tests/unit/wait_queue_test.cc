#include "support/runtime_fixture.h"
#include <algorithm>
#include <numeric>
#include <random>
using namespace zco;

TEST(WaitQueue, ArbitraryRemovalAndRepeatedRemovalLeaveNoWaiters) {
    detail::WaitQueue queue;
    std::vector<detail::WaitTicket> tickets;
    for (int i = 0; i < 8192; ++i)
        tickets.push_back(queue.add());
    std::vector<size_t> order(tickets.size());
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937(17));
    for (auto i : order) {
        queue.remove(tickets[i]);
        queue.remove(tickets[i]);
    }
    EXPECT_FALSE(queue.complete_one());
}

TEST(WaitQueue, RemovalPreservesFifoAndCompletedTicketsCanBeRemoved) {
    detail::WaitQueue queue;
    auto first = queue.add();
    auto canceled = queue.add();
    auto second = queue.add();
    queue.remove(canceled);
    EXPECT_TRUE(queue.complete_one());
    EXPECT_EQ(first.wait(test::ms(0)), WaitOutcome::ready);
    auto copy = first;
    queue.remove(copy);
    queue.remove(first);
    EXPECT_TRUE(queue.complete_one(WaitOutcome::closed));
    EXPECT_EQ(second.wait(test::ms(0)), WaitOutcome::closed);
    queue.remove(second);
    EXPECT_FALSE(queue.complete_one());
    EXPECT_EQ(canceled.wait(test::ms(0)), WaitOutcome::timeout);
}

TEST(WaitQueue, ExpiredAndTimedOutTicketsDoNotConsumeACompletion) {
    detail::WaitQueue queue;
    WakeToken expired;
    {
        auto ticket = queue.add();
        expired = ticket.token();
    }
    EXPECT_FALSE(expired.complete());
    auto timed_out = queue.add();
    EXPECT_EQ(timed_out.wait(test::ms(0)), WaitOutcome::timeout);
    auto next = queue.add();
    EXPECT_TRUE(queue.complete_one());
    EXPECT_EQ(next.wait(test::ms(0)), WaitOutcome::ready);
    queue.remove(timed_out);
    queue.remove(next);
    EXPECT_FALSE(queue.complete_one());
}

TEST(WaitQueue, QueueDestructionInvalidatesLiveRegistrations) {
    detail::WaitTicket survivor;
    {
        detail::WaitQueue queue;
        survivor = queue.add();
    }
    detail::WaitQueue next;
    next.remove(survivor);
    auto ticket = next.add();
    EXPECT_TRUE(next.complete_one());
    EXPECT_EQ(ticket.wait(test::ms(0)), WaitOutcome::ready);
}

TEST(WaitQueue, SignalAndTimeoutRaceWithRemovalUnderThePredicateMutex) {
    for (int iteration = 0; iteration < 200; ++iteration) {
        detail::WaitQueue queue;
        std::mutex mutex;
        auto ticket = queue.add();
        std::thread signal([&] {
            std::lock_guard<std::mutex> lock(mutex);
            queue.complete_one();
        });
        auto outcome = ticket.wait(test::ms(0));
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.remove(ticket);
        }
        signal.join();
        EXPECT_TRUE(outcome == WaitOutcome::ready ||
                    outcome == WaitOutcome::timeout);
        EXPECT_FALSE(queue.complete_one());
        EXPECT_FALSE(ticket.token().complete(WaitOutcome::closed));
    }
}

TEST(WaitQueue, CoroutineTimeoutStormAndSignalLeaveTheEventReusable) {
    for (auto model : {StackModel::kShared, StackModel::kIndependent}) {
        RuntimeOptions options{2};
        options.stack_model = model;
        Runtime runtime(options);
        Event event(true);
        std::atomic<int> entered{0}, returned{0};
        std::vector<TaskHandle> tasks;
        for (int i = 0; i < 2000; ++i)
            tasks.push_back(test::spawn(runtime, [&] {
                ++entered;
                auto result = event.wait(test::ms(5));
                EXPECT_TRUE(result || result.error() ==
                                          wait_error(WaitOutcome::timeout));
                ++returned;
            }));
        while (entered != 2000)
            std::this_thread::yield();
        event.signal();
        for (auto &task : tasks)
            EXPECT_TRUE(task.join(test::soon()));
        EXPECT_EQ(returned, 2000);
        event.reset();
        EXPECT_FALSE(event.wait(test::ms(0)));
        event.signal();
        EXPECT_TRUE(event.wait(test::ms(0)));
    }
}
