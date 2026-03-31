#include <deque>
#include <vector>

#include <gtest/gtest.h>

#include "zcoroutine/internal/coroutine_waiter.h"
#include "zcoroutine/internal/processor.h"
#include "support/internal_fiber_test_helper.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class CoroutineWaiterUnitTest : public test::RuntimeTestBase {};

TEST_F(CoroutineWaiterUnitTest, ValidityChecksHandleDifferentEntryStates) {
  Processor processor(0, 64 * 1024);
  std::shared_ptr<Fiber> fiber = test::MakeFiberForTest(&processor, 1, 0);

  CoroutineWaiterEntry missing_active;
  missing_active.coroutine = fiber;
  EXPECT_FALSE(is_waiter_entry_valid(missing_active));

  CoroutineWaiterEntry inactive;
  inactive.coroutine = fiber;
  inactive.active = std::make_shared<std::atomic<bool>>(false);
  EXPECT_FALSE(is_waiter_entry_valid(inactive));

  CoroutineWaiterEntry valid;
  valid.coroutine = fiber;
  valid.active = std::make_shared<std::atomic<bool>>(true);
  EXPECT_TRUE(is_waiter_entry_valid(valid));

  fiber.reset();
  EXPECT_FALSE(is_waiter_entry_valid(valid));
}

TEST_F(CoroutineWaiterUnitTest, CleanupFunctionsRemoveInvalidEntries) {
  Processor processor(0, 64 * 1024);
  std::shared_ptr<Fiber> fiber = test::MakeFiberForTest(&processor, 2, 0);

  CoroutineWaiterEntry valid;
  valid.coroutine = fiber;
  valid.active = std::make_shared<std::atomic<bool>>(true);

  CoroutineWaiterEntry invalid;
  invalid.coroutine = fiber;
  invalid.active = std::make_shared<std::atomic<bool>>(false);

  std::vector<CoroutineWaiterEntry> vec;
  vec.push_back(invalid);
  vec.push_back(valid);
  cleanup_waiters(&vec);
  ASSERT_EQ(vec.size(), 1u);
  EXPECT_TRUE(is_waiter_entry_valid(vec.front()));

  std::deque<CoroutineWaiterEntry> dq;
  dq.push_back(invalid);
  dq.push_back(valid);
  cleanup_waiters_front(&dq);
  ASSERT_EQ(dq.size(), 1u);
  EXPECT_TRUE(is_waiter_entry_valid(dq.front()));
}

TEST_F(CoroutineWaiterUnitTest, ClaimWaiterReturnsFiberOnlyOnce) {
  Processor processor(0, 64 * 1024);
  std::shared_ptr<Fiber> fiber = test::MakeFiberForTest(&processor, 3, 0);

  CoroutineWaiterEntry waiter;
  waiter.coroutine = fiber;
  waiter.active = std::make_shared<std::atomic<bool>>(true);

  std::shared_ptr<Fiber> first = claim_waiter(&waiter);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first.get(), fiber.get());

  EXPECT_EQ(claim_waiter(&waiter), nullptr);
  waiter.active.reset();
  EXPECT_EQ(claim_waiter(&waiter), nullptr);
  EXPECT_EQ(claim_waiter(nullptr), nullptr);
}

TEST_F(CoroutineWaiterUnitTest, CleanupWaitersFrontKeepsDequeWhenFrontIsValid) {
  Processor processor(0, 64 * 1024);
  std::shared_ptr<Fiber> fiber = test::MakeFiberForTest(&processor, 4, 0);

  CoroutineWaiterEntry valid_front;
  valid_front.coroutine = fiber;
  valid_front.active = std::make_shared<std::atomic<bool>>(true);

  CoroutineWaiterEntry invalid_tail;
  invalid_tail.coroutine = fiber;
  invalid_tail.active = std::make_shared<std::atomic<bool>>(false);

  std::deque<CoroutineWaiterEntry> dq;
  dq.push_back(valid_front);
  dq.push_back(invalid_tail);

  cleanup_waiters_front(&dq);
  ASSERT_EQ(dq.size(), 2u);
  EXPECT_TRUE(is_waiter_entry_valid(dq.front()));
}

}  // namespace
}  // namespace zcoroutine
