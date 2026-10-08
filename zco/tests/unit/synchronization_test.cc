#include "support/runtime_fixture.h"
using namespace zco;

TEST(Synchronization, EventAutoResetAndManualReset) {
    Event automatic;
    automatic.signal();
    EXPECT_TRUE(automatic.wait(test::ms(0)));
    EXPECT_FALSE(automatic.wait(test::ms(0)));
    Event manual(true, true);
    EXPECT_TRUE(manual.wait(test::ms(0)));
    EXPECT_TRUE(manual.wait(test::ms(0)));
    manual.reset();
    EXPECT_FALSE(manual.wait(test::ms(0)));
}

TEST(Synchronization, ThreadsAndCoroutinesShareEventAndMutex) {
    Runtime runtime(RuntimeOptions{2});
    Mutex mutex;
    int count = 0;
    std::vector<TaskHandle> tasks;
    for (int n = 0; n < 20; ++n)
        tasks.push_back(test::spawn(runtime, [&] {
            for (int i = 0; i < 100; ++i) {
                ASSERT_TRUE(mutex.lock());
                ++count;
                yield();
                mutex.unlock();
            }
        }));
    std::thread thread([&] {
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(mutex.lock());
            ++count;
            mutex.unlock();
        }
    });
    for (auto &task : tasks)
        EXPECT_TRUE(task.join(test::soon()));
    thread.join();
    EXPECT_EQ(count, 2100);
    EXPECT_THROW(mutex.unlock(), std::logic_error);
}

TEST(Synchronization, TimedMutexWaitDoesNotAcquireOwnership) {
    Runtime runtime(RuntimeOptions{1});
    Mutex mutex;
    EXPECT_TRUE(mutex.try_lock());
    auto task =
        test::spawn(runtime, [&] { EXPECT_FALSE(mutex.lock(test::ms(0))); });
    EXPECT_TRUE(task.join(test::soon()));
    EXPECT_FALSE(mutex.try_lock());
    mutex.unlock();
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(Synchronization, WaitGroupHasOnePredicateDuringAddDone) {
    WaitGroup group(1);
    std::thread first([&] {
        for (int i = 0; i < 5000; ++i) {
            group.add();
            group.done();
        }
    });
    std::thread second([&] {
        for (int i = 0; i < 5000; ++i) {
            group.add();
            group.done();
        }
    });
    first.join();
    second.join();
    group.done();
    EXPECT_TRUE(group.wait(test::ms(0)));
    EXPECT_THROW(group.done(), std::logic_error);
}

TEST(Synchronization, ChannelSupportsMoveOnlyValuesPerCallResultsAndClose) {
    Channel<std::unique_ptr<int>> channel(2);
    EXPECT_TRUE(channel.send(std::unique_ptr<int>(new int(42))));
    auto result = channel.receive();
    ASSERT_TRUE(result);
    EXPECT_EQ(*result.value(), 42);
    channel.close();
    EXPECT_FALSE(channel.send(std::unique_ptr<int>(new int(1))));
    EXPECT_FALSE(channel.receive());
}

TEST(Synchronization, ChannelMixedWaitersAndBudget) {
    Runtime runtime(RuntimeOptions{2});
    Channel<int> channel(1);
    auto producer = test::spawn(runtime, [&] {
        for (int i = 0; i < 500; ++i)
            EXPECT_TRUE(channel.send(i));
        channel.close();
    });
    int count = 0;
    while (auto value = channel.receive(test::soon()))
        EXPECT_EQ(value.value(), count++);
    EXPECT_TRUE(producer.join(test::soon()));
    EXPECT_EQ(count, 500);
    Channel<int> full(1);
    EXPECT_TRUE(full.send(1));
    auto timeout = full.send(2, test::ms(0));
    EXPECT_EQ(timeout.error(), wait_error(WaitOutcome::timeout));
}
