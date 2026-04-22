#include <atomic>
#include <chrono>
#include <deque>
#include <sys/epoll.h>
#include <thread>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/processor.h"
#include "zco/internal/runtime_manager.h"

namespace zco {
namespace {

class ProcessorUnitTest : public test::RuntimeTestBase {};

TEST_F(ProcessorUnitTest, BasicPropertiesAndSharedStackBounds) {
    Processor processor(7, 64 * 1024);

    EXPECT_EQ(processor.id(), 7);
    EXPECT_GE(processor.shared_stack_count(), 1u);
    EXPECT_NE(processor.shared_stack_data(0), nullptr);
    EXPECT_GT(processor.shared_stack_size(0), 0u);

    const size_t out_of_range = processor.shared_stack_count() + 1;
    EXPECT_EQ(processor.shared_stack_data(out_of_range), nullptr);
    EXPECT_EQ(processor.shared_stack_size(out_of_range), 0u);
}

TEST_F(ProcessorUnitTest, PendingTaskCountAndStealFlow) {
    Processor processor(1, 64 * 1024);
    EXPECT_EQ(processor.pending_task_count(), 0u);

    processor.enqueue_task([]() {});
    processor.enqueue_task([]() {});
    processor.enqueue_task([]() {});
    EXPECT_EQ(processor.pending_task_count(), 3u);

    std::deque<Task> stolen;
    const size_t count = processor.steal_tasks(&stolen, 2, 0);
    EXPECT_GE(count, 1u);
    EXPECT_LE(count, 2u);
    EXPECT_EQ(stolen.size(), count);
}

TEST_F(ProcessorUnitTest, StartStopJoinAndTimerApisAreSafe) {
    Processor processor(2, 64 * 1024);

    std::shared_ptr<TimerToken> token = processor.add_timer(10, []() {});
    ASSERT_NE(token, nullptr);

    processor.start();
    processor.stop();
    processor.join();

    processor.stop();
    processor.join();
}

TEST_F(ProcessorUnitTest, QueueAndLoadMetricsAreQueryable) {
    Processor processor(3, 64 * 1024);

    EXPECT_EQ(processor.queue_load(), 0u);
    EXPECT_GE(processor.cpu_time_ns(), 0u);
    EXPECT_GE(processor.load_score(), 0u);

    processor.enqueue_task([]() {});
    EXPECT_GE(processor.queue_load(), 1u);
}

TEST_F(ProcessorUnitTest, IndependentStackModelHasNoSharedStackPool) {
    Processor processor(9, 64 * 1024, 6, StackModel::kIndependent);

    EXPECT_EQ(processor.stack_model(), StackModel::kIndependent);
    EXPECT_EQ(processor.shared_stack_count(), 0u);
    EXPECT_EQ(processor.shared_stack_data(0), nullptr);
    EXPECT_EQ(processor.shared_stack_size(0), 0u);
}

TEST_F(ProcessorUnitTest, SharedStackModelUsesConfiguredPoolSize) {
    Processor processor(10, 32 * 1024, 3, StackModel::kShared);

    EXPECT_EQ(processor.stack_model(), StackModel::kShared);
    EXPECT_EQ(processor.shared_stack_count(), 3u);
    EXPECT_NE(processor.shared_stack_data(0), nullptr);
    EXPECT_EQ(processor.shared_stack_size(0), 32u * 1024u);
    EXPECT_EQ(processor.shared_stack_data(3), nullptr);
    EXPECT_EQ(processor.shared_stack_size(3), 0u);
}

TEST_F(ProcessorUnitTest, SharedStackModelWithZeroStackNumFallsBackToOne) {
    Processor processor(11, 32 * 1024, 0, StackModel::kShared);

    EXPECT_EQ(processor.stack_model(), StackModel::kShared);
    EXPECT_EQ(processor.shared_stack_count(), 1u);
    EXPECT_NE(processor.shared_stack_data(0), nullptr);
    EXPECT_EQ(processor.shared_stack_size(0), 32u * 1024u);
}

TEST_F(ProcessorUnitTest, NullAndNoCurrentFiberPathsReturnSafely) {
    Processor processor(12, 64 * 1024);

    processor.enqueue_ready(Fiber::ptr());
    processor.yield_current();
    processor.prepare_wait_current();
    EXPECT_FALSE(processor.park_current());
    EXPECT_FALSE(processor.park_current_for(1));
    EXPECT_FALSE(processor.wait_fd(3, EPOLLIN, 1));

    std::deque<Task> out;
    EXPECT_EQ(processor.steal_tasks(nullptr, 8, 0), 0u);
    EXPECT_EQ(processor.steal_tasks(&out, 0, 0), 0u);
}

TEST_F(ProcessorUnitTest, RunLoopExecutesEnqueuedTaskToCompletion) {
    Processor processor(13, 64 * 1024);
    std::atomic<int> counter(0);

    processor.start();
    processor.enqueue_task(
        [&counter]() { counter.fetch_add(1, std::memory_order_release); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (counter.load(std::memory_order_acquire) != 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
    EXPECT_GE(processor.cpu_time_ns(), 0u);

    processor.stop();
    processor.join();
}

TEST_F(ProcessorUnitTest, YieldingTaskIsRescheduledAndEventuallyCompletes) {
    Processor processor(14, 64 * 1024);
    std::atomic<int> phase(0);

    processor.start();
    processor.enqueue_task([&phase]() {
        phase.fetch_add(1, std::memory_order_release);
        yield();
        phase.fetch_add(1, std::memory_order_release);
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (phase.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(phase.load(std::memory_order_acquire), 2);
    EXPECT_GE(processor.load_score(), 0u);

    processor.stop();
    processor.join();
}

TEST_F(ProcessorUnitTest, IdleProcessorStealsPendingTasksFromBusyVictim) {
    Runtime &runtime = Runtime::instance();
    runtime.init(2);
    ASSERT_EQ(runtime.scheduler_count(), 2u);

    std::atomic<bool> blocker_started(false);
    std::atomic<bool> release_blocker(false);

    constexpr int kTaskCount = 600;
    WaitGroup done(kTaskCount + 1);
    std::atomic<int> ran_on_sched0(0);
    std::atomic<int> ran_on_other(0);

    runtime.submit_to(0, [&blocker_started, &release_blocker, &done]() {
        blocker_started.store(true, std::memory_order_release);
        while (!release_blocker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        done.done();
    });

    const auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!blocker_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(blocker_started.load(std::memory_order_acquire));

    for (int i = 0; i < kTaskCount; ++i) {
        runtime.submit_to(0, [&ran_on_sched0, &ran_on_other, &done]() {
            const int sid = sched_id();
            if (sid == 0) {
                ran_on_sched0.fetch_add(1, std::memory_order_relaxed);
            } else {
                ran_on_other.fetch_add(1, std::memory_order_relaxed);
            }
            done.done();
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    release_blocker.store(true, std::memory_order_release);
    done.wait();

    EXPECT_EQ(ran_on_sched0.load(std::memory_order_relaxed) +
                  ran_on_other.load(std::memory_order_relaxed),
              kTaskCount);
    EXPECT_GT(ran_on_other.load(std::memory_order_relaxed), 0);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
