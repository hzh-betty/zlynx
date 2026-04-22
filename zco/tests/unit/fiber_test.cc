#include <cstring>
#include <stdexcept>

#include <gtest/gtest.h>

#include "support/internal_fiber_test_helper.h"
#include "support/test_fixture.h"
#include "zco/internal/fiber.h"
#include "zco/internal/processor.h"

namespace zco {
namespace {

class FiberUnitTest : public test::RuntimeTestBase {};

TEST_F(FiberUnitTest, BasicStateTransitionsAndTimeoutFlags) {
    Processor processor(0, 64 * 1024);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 1, 0);

    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->id(), 1);
    EXPECT_EQ(fiber->owner(), &processor);
    EXPECT_EQ(fiber->stack_slot(), 0u);
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    EXPECT_FALSE(fiber->timed_out());

    fiber->mark_running();
    EXPECT_EQ(fiber->state(), Fiber::State::kRunning);

    fiber->mark_waiting();
    EXPECT_EQ(fiber->state(), Fiber::State::kWaiting);
    EXPECT_TRUE(fiber->try_wake(false));
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    EXPECT_FALSE(fiber->timed_out());

    fiber->mark_waiting();
    EXPECT_TRUE(fiber->try_wake(true));
    EXPECT_TRUE(fiber->timed_out());
    fiber->clear_timed_out();
    EXPECT_FALSE(fiber->timed_out());

    fiber->mark_done();
    EXPECT_EQ(fiber->state(), Fiber::State::kDone);
    EXPECT_FALSE(fiber->try_wake(false));
}

TEST_F(FiberUnitTest, RunMarksDoneEvenIfTaskThrows) {
    Processor processor(0, 64 * 1024);
    bool entered = false;
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 2, 0, [&entered]() {
        entered = true;
        throw std::runtime_error("expected in test");
    });

    fiber->run();
    EXPECT_TRUE(entered);
    EXPECT_EQ(fiber->state(), Fiber::State::kDone);
}

TEST_F(FiberUnitTest, SaveAndClearSnapshotWorks) {
    Processor processor(0, 64 * 1024);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 3, 0);

    const char payload[] = "snapshot-payload";
    fiber->save_stack_data(payload, sizeof(payload));

    ASSERT_TRUE(fiber->has_saved_stack());
    ASSERT_EQ(fiber->saved_stack_size(), sizeof(payload));
    ASSERT_NE(fiber->saved_stack_data(), nullptr);
    EXPECT_EQ(std::memcmp(fiber->saved_stack_data(), payload, sizeof(payload)),
              0);

    fiber->clear_saved_stack();
    EXPECT_FALSE(fiber->has_saved_stack());
    EXPECT_EQ(fiber->saved_stack_data(), nullptr);
}

TEST_F(FiberUnitTest, ResetReinitializesFiberIdentityAndState) {
    Processor processor(0, 64 * 1024);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 5, 0);

    const char payload[] = "x";
    fiber->save_stack_data(payload, sizeof(payload));
    fiber->mark_waiting();
    EXPECT_TRUE(fiber->has_saved_stack());

    fiber->reset(9, []() {}, 1);

    EXPECT_EQ(fiber->id(), 9);
    EXPECT_EQ(fiber->stack_slot(), 1u);
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    EXPECT_FALSE(fiber->timed_out());
    EXPECT_FALSE(fiber->has_saved_stack());
}

TEST_F(FiberUnitTest, IndependentStackModeDisablesSnapshotStorage) {
    Processor processor(0, 64 * 1024, 4, StackModel::kIndependent);
    Fiber::ptr fiber =
        test::MakeFiberForTest(&processor, 7, 0, Task(), false, 64 * 1024);

    ASSERT_NE(fiber, nullptr);
    EXPECT_FALSE(fiber->use_shared_stack());

    const char payload[] = "independent-stack";
    fiber->save_stack_data(payload, sizeof(payload));

    EXPECT_FALSE(fiber->has_saved_stack());
    EXPECT_EQ(fiber->saved_stack_size(), 0u);
    EXPECT_EQ(fiber->saved_stack_data(), nullptr);
}

TEST_F(FiberUnitTest, IndependentStackContextInitializeIsIdempotent) {
    Processor processor(0, 64 * 1024, 4, StackModel::kIndependent);
    Fiber::ptr fiber =
        test::MakeFiberForTest(&processor, 8, 0, Task(), false, 64 * 1024);

    ASSERT_NE(fiber, nullptr);
    EXPECT_FALSE(fiber->context_initialized());

    fiber->initialize_context();
    EXPECT_TRUE(fiber->context_initialized());

    fiber->initialize_context();
    EXPECT_TRUE(fiber->context_initialized());
}

TEST_F(FiberUnitTest, IndependentStackRequiresPositiveStackSize) {
    Processor processor(0, 64 * 1024, 4, StackModel::kIndependent);

    EXPECT_THROW(
        {
            Fiber::ptr fiber = std::make_shared<Fiber>(
                11, &processor, Task([]() {}), 0, 0, false);
            (void)fiber;
        },
        std::runtime_error);
}

TEST_F(FiberUnitTest, SharedStackRejectsOutOfRangeSlot) {
    Processor processor(0, 64 * 1024, 2, StackModel::kShared);

    EXPECT_THROW(
        {
            Fiber::ptr fiber = std::make_shared<Fiber>(
                13, &processor, Task([]() {}), 64 * 1024, 7, true);
            (void)fiber;
        },
        std::runtime_error);
}

TEST_F(FiberUnitTest, SharedStackContextInitializeIsIdempotent) {
    Processor processor(0, 64 * 1024, 4, StackModel::kShared);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 14, 1, Task(), true);

    ASSERT_NE(fiber, nullptr);
    ASSERT_TRUE(fiber->use_shared_stack());
    EXPECT_FALSE(fiber->context_initialized());

    fiber->initialize_context();
    EXPECT_TRUE(fiber->context_initialized());

    fiber->initialize_context();
    EXPECT_TRUE(fiber->context_initialized());
}

TEST_F(FiberUnitTest, SharedStackZeroSizeSnapshotClearsSavedData) {
    Processor processor(0, 64 * 1024, 4, StackModel::kShared);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 15, 0);

    ASSERT_NE(fiber, nullptr);
    const char payload[] = "snapshot";
    fiber->save_stack_data(payload, sizeof(payload));
    ASSERT_TRUE(fiber->has_saved_stack());

    fiber->save_stack_data(payload, 0);
    EXPECT_FALSE(fiber->has_saved_stack());
    EXPECT_EQ(fiber->saved_stack_size(), 0u);
    EXPECT_EQ(fiber->saved_stack_data(), nullptr);
}

TEST_F(FiberUnitTest, IndependentStackAllowsOutOfRangeStackSlot) {
    Processor processor(0, 64 * 1024, 0, StackModel::kIndependent);

    Fiber::ptr fiber =
        test::MakeFiberForTest(&processor, 12, 999, Task(), false, 32 * 1024);

    ASSERT_NE(fiber, nullptr);
    EXPECT_FALSE(fiber->use_shared_stack());
    EXPECT_EQ(fiber->stack_slot(), 999u);

    fiber->initialize_context();
    EXPECT_TRUE(fiber->context_initialized());
}

TEST_F(FiberUnitTest, NullOwnerIsRejected) {
    EXPECT_THROW(
        {
            Fiber::ptr fiber = std::make_shared<Fiber>(
                21, nullptr, Task([]() {}), 64 * 1024, 0, true);
            (void)fiber;
        },
        std::runtime_error);
}

TEST_F(FiberUnitTest, TrySetExternalHandleRejectsInvalidInputs) {
    Processor processor(0, 64 * 1024);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 22, 0);
    ASSERT_NE(fiber, nullptr);

    uint64_t effective = 0;
    EXPECT_FALSE(fiber->try_set_external_handle_id(0, &effective));
    EXPECT_FALSE(fiber->try_set_external_handle_id(123, nullptr));
    EXPECT_EQ(fiber->external_handle_id(), 0u);

    EXPECT_TRUE(fiber->try_set_external_handle_id(123, &effective));
    EXPECT_EQ(effective, 123u);
    EXPECT_EQ(fiber->external_handle_id(), 123u);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
