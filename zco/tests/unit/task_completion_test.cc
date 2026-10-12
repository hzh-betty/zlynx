#include "runtime/task_queues.h"
#include "support/runtime_fixture.h"
#include <array>
using namespace zco;

namespace {
thread_local bool count_allocations = false;
thread_local size_t allocations = 0;
thread_local int fail_after = -1;
} // namespace

extern "C" void *__real__Znwm(size_t);

extern "C" void *__wrap__Znwm(size_t size) {
    if (count_allocations)
        ++allocations;
    if (fail_after >= 0 && fail_after-- == 0)
        throw std::bad_alloc();
    return __real__Znwm(size);
}

TEST(TaskCompletion, UnobservedTaskAndCompletedJoinNeedNoWaitAllocations) {
    allocations = 0;
    count_allocations = true;
    auto completion = std::make_shared<detail::Completion>(TaskId{1});
    completion->finish(TaskStatus::succeeded);
    auto result = TaskHandle(completion).join();
    count_allocations = false;
    EXPECT_TRUE(result);

    // 只分配共享完成状态，不分配等待存储。
    EXPECT_EQ(allocations, 1u);
}

TEST(TaskCompletion, FirstJoinAllocationFailureAllowsRetryAndCompletion) {
    // 等待状态和注册节点的分配都可能独立失败。
    for (int allocation = 0; allocation < 2; ++allocation) {
        auto completion = std::make_shared<detail::Completion>(TaskId{1});
        TaskHandle handle(completion);
        bool failed = false;
        fail_after = allocation;
        try {
            (void)handle.join(test::ms(0));
        } catch (const std::bad_alloc &) {
            failed = true;
        }
        fail_after = -1;
        EXPECT_TRUE(failed) << allocation;
        EXPECT_EQ(handle.status(), TaskStatus::pending);
        auto timeout = handle.join(test::ms(0));
        EXPECT_EQ(timeout.error(), wait_error(WaitOutcome::timeout));
        completion->finish(TaskStatus::succeeded);
        EXPECT_TRUE(handle.join(test::soon()));
    }
}

TEST(TaskCompletion, FinishRacesFirstRegistrationAndConcurrentJoiners) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto completion = std::make_shared<detail::Completion>(TaskId{1});
        TaskHandle handle(completion);
        std::atomic<int> ready{0}, joined{0};
        std::atomic<bool> go{false};
        std::array<std::thread, 4> threads;
        for (auto &thread : threads)
            thread = std::thread([&] {
                ++ready;
                EXPECT_TRUE(test::wait_until([&] { return go.load(); }));
                if (handle.join(test::soon()))
                    ++joined;
            });
        EXPECT_TRUE(test::wait_until([&] { return ready == 4; }));
        go = true;
        completion->finish(TaskStatus::succeeded);
        for (auto &thread : threads)
            thread.join();
        EXPECT_EQ(joined, 4);
    }
}

TEST(TaskCompletion, MultipleCoroutineJoinersObserveTheSameResult) {
    for (auto model : {StackModel::kShared, StackModel::kIndependent}) {
        RuntimeOptions options{2};
        options.stack_model = model;
        Runtime runtime(options);
        Event release;
        auto target = test::spawn(runtime.executor(0), [&] {
            release.wait(test::soon()).value();
            throw std::runtime_error("task failure");
        });
        std::atomic<int> entered{0}, observed{0};
        std::vector<TaskHandle> joiners;
        for (int i = 0; i < 16; ++i)
            joiners.push_back(test::spawn(runtime.executor(1), [&] {
                ++entered;
                try {
                    target.join(test::soon()).value();
                } catch (const std::runtime_error &error) {
                    EXPECT_STREQ(error.what(), "task failure");
                    ++observed;
                }
            }));

        // 固定到工作线程的标记仅在此前所有等待者挂起后运行。
        auto marker = test::spawn(runtime.executor(1), [] {});
        EXPECT_TRUE(marker.join(test::soon()));
        EXPECT_EQ(entered, 16);
        release.signal();
        for (auto &joiner : joiners)
            EXPECT_TRUE(joiner.join(test::soon()));
        EXPECT_EQ(observed, 16);
        EXPECT_EQ(target.status(), TaskStatus::failed);
    }
}

TEST(TaskCompletion, TimedOutJoinCanBeRetriedFromACoroutine) {
    for (auto model : {StackModel::kShared, StackModel::kIndependent}) {
        RuntimeOptions options{1};
        options.stack_model = model;
        Runtime runtime(options);
        Event release;
        auto target =
            test::spawn(runtime, [&] { release.wait(test::soon()).value(); });
        auto joiner = test::spawn(runtime, [&] {
            auto timeout = target.join(test::ms(0));
            EXPECT_EQ(timeout.error(), wait_error(WaitOutcome::timeout));
            release.signal();
            EXPECT_TRUE(target.join(test::soon()));
            EXPECT_TRUE(target.join(test::ms(0)));
        });
        EXPECT_TRUE(joiner.join(test::soon()));
    }
}
