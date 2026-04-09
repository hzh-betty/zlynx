#ifndef ZCOROUTINE_TESTS_SUPPORT_TEST_FIXTURE_H_
#define ZCOROUTINE_TESTS_SUPPORT_TEST_FIXTURE_H_

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "zcoroutine/zcoroutine.h"

namespace zcoroutine {
namespace test {

class RuntimeTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    shutdown();
    co_stack_num(8);
    co_stack_size(128 * 1024);
    co_stack_model(StackModel::kShared);
  }

  void TearDown() override { shutdown(); }

  static void WaitShortly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
};

}  // namespace test
}  // namespace zcoroutine

#endif  // ZCOROUTINE_TESTS_SUPPORT_TEST_FIXTURE_H_
