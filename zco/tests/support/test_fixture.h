#ifndef ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_
#define ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "zco/zco.h"

namespace zco {
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

} // namespace test
} // namespace zco

#endif // ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_
