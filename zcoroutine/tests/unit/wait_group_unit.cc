#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "zcoroutine/wait_group.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class WaitGroupUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(WaitGroupUnitByHeaderTest, AddDoneAndWaitFlow) {
  WaitGroup group(0);
  group.add(2);

  go([&group]() {
    co_sleep_for(1);
    group.done();
  });

  go([&group]() {
    co_sleep_for(1);
    group.done();
  });

  group.wait();
  SUCCEED();
}

TEST_F(WaitGroupUnitByHeaderTest, DoneWithoutOutstandingCountThrows) {
  WaitGroup group(0);
  EXPECT_THROW(group.done(), std::runtime_error);
}

TEST_F(WaitGroupUnitByHeaderTest, AddZeroKeepsWaitImmediatelyReady) {
  WaitGroup group(0);

  group.add(0);
  group.wait();
  SUCCEED();
}

TEST_F(WaitGroupUnitByHeaderTest, AddFromZeroBlocksUntilDoneThenCanReuse) {
  WaitGroup group(0);
  std::atomic<bool> waited(false);

  group.add(1);
  std::thread waiter([&group, &waited]() {
    group.wait();
    waited.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  EXPECT_FALSE(waited.load(std::memory_order_acquire));

  group.done();
  waiter.join();
  EXPECT_TRUE(waited.load(std::memory_order_acquire));

  // 计数归零后应可继续复用。
  waited.store(false, std::memory_order_release);
  group.add(1);
  std::thread waiter2([&group, &waited]() {
    group.wait();
    waited.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  EXPECT_FALSE(waited.load(std::memory_order_acquire));
  group.done();
  waiter2.join();
  EXPECT_TRUE(waited.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace zcoroutine
