#include <atomic>
#include <deque>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/fiber.h"
#include "zco/internal/timer.h"

#define private public
#include "zco/internal/processor.h"
#undef private

namespace zco {
namespace {

class ProcessorInternalUnitTest : public test::RuntimeTestBase {};

TEST_F(ProcessorInternalUnitTest,
       DrainReadyAndBatchGuardsCoverNullAndEmptyInputs) {
    Processor processor(21, 64 * 1024);

    std::deque<Fiber::ptr> drained;
    EXPECT_EQ(processor.drain_ready_fibers(nullptr, 4), 0u);
    EXPECT_EQ(processor.drain_ready_fibers(&drained, 0), 0u);
    EXPECT_EQ(processor.drain_ready_fibers(&drained, 2), 0u);

    processor.enqueue_ready_batch(nullptr);

    std::deque<Fiber::ptr> ready_batch;
    processor.enqueue_ready_batch(&ready_batch);
    EXPECT_TRUE(ready_batch.empty());

    Fiber::ptr fiber =
        std::make_shared<Fiber>(101, &processor, []() {}, 64 * 1024, 0, true);
    ready_batch.push_back(fiber);
    processor.enqueue_ready_batch(&ready_batch);
    EXPECT_TRUE(ready_batch.empty());

    EXPECT_EQ(processor.drain_ready_fibers(&drained, 1), 1u);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained.front()->id(), 101);
}

TEST_F(ProcessorInternalUnitTest, RecycleAndIoReadyGuardPathsAreExercised) {
    Processor processor(22, 64 * 1024);

    Fiber::ptr fiber =
        std::make_shared<Fiber>(102, &processor, []() {}, 64 * 1024, 0, true);
    EXPECT_FALSE(processor.recycle_if_done_before_run(fiber));

    fiber->mark_done();
    EXPECT_TRUE(processor.recycle_if_done_before_run(fiber));

    processor.handle_io_ready(nullptr, 0);

    std::shared_ptr<IoWaiter> inactive = std::make_shared<IoWaiter>();
    inactive->fd = 3;
    inactive->events = 0;
    inactive->active.store(false, std::memory_order_release);
    processor.handle_io_ready(inactive, 0);

    std::shared_ptr<IoWaiter> ready = std::make_shared<IoWaiter>();
    ready->fd = 4;
    ready->events = 0;
    ready->timer = std::make_shared<TimerToken>();
    ready->active.store(true, std::memory_order_release);
    {
        Fiber::ptr tmp = std::make_shared<Fiber>(
            103, &processor, []() {}, 64 * 1024, 0, true);
        ready->fiber = tmp;
    }

    processor.handle_io_ready(ready, 0);
    EXPECT_FALSE(ready->active.load(std::memory_order_acquire));
    ASSERT_NE(ready->timer, nullptr);
    EXPECT_TRUE(ready->timer->cancelled.load(std::memory_order_acquire));
}

TEST_F(ProcessorInternalUnitTest, RunLoopAndWaitIoGuardHandleMissingPoller) {
    Processor processor(23, 64 * 1024);
    processor.poller_.reset();

    processor.wait_io_events_when_idle();

    processor.running_.store(true, std::memory_order_release);
    processor.run_loop();

    EXPECT_FALSE(processor.running_.load(std::memory_order_acquire));
    EXPECT_EQ(current_processor(), nullptr);
}

TEST_F(ProcessorInternalUnitTest,
       SaveRestoreEarlyReturnWhenSharedStackStorageUnavailable) {
    Processor processor(24, 64 * 1024, 1, StackModel::kIndependent);
    Fiber::ptr fiber =
        std::make_shared<Fiber>(104, &processor, []() {}, 64 * 1024, 0, false);

    processor.save_fiber_stack(fiber);

    const char payload[] = {'a', 'b', 'c', 'd'};
    fiber->save_stack_data(payload, sizeof(payload));
    const size_t before = fiber->saved_stack_size();
    processor.restore_fiber_stack(fiber);

    EXPECT_EQ(fiber->saved_stack_size(), before);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
