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
  void SetUp() override { shutdown(); }

  void TearDown() override { shutdown(); }

  static void WaitShortly() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
};

}  // namespace test
}  // namespace zcoroutine

#endif  // ZCOROUTINE_TESTS_SUPPORT_TEST_FIXTURE_H_
