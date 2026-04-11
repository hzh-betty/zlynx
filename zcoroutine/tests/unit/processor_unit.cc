#include <deque>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zcoroutine/internal/processor.h"

namespace zcoroutine {
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

} // namespace
} // namespace zcoroutine
