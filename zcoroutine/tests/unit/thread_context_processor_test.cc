/**
 * @file thread_context_processor_test.cc
 * @brief ThreadContext Processor 绑定相关单元测试
 */

#include "processor.h"
#include "thread_context.h"
#include "zcoroutine_logger.h"

#include <gtest/gtest.h>

using namespace zcoroutine;

class ThreadContextProcessorTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  }

  void TearDown() override {
    // 尽量清理 TLS，避免测试间相互影响。
    ThreadContext::set_processor(nullptr);
    ThreadContext::set_scheduler(nullptr);
    ThreadContext::set_main_fiber(nullptr);
    ThreadContext::set_scheduler_fiber(nullptr);
    ThreadContext::set_current_fiber(nullptr);
  }
};

TEST_F(ThreadContextProcessorTest, SetProcessorDerivesWorkerId) {
  Processor p0(7);

  ThreadContext::set_processor(&p0);

  EXPECT_EQ(ThreadContext::get_processor(), &p0);
  EXPECT_EQ(ThreadContext::get_worker_id(), 7);
}

TEST_F(ThreadContextProcessorTest, ClearProcessorResetsWorkerId) {
  Processor p0(3);
  ThreadContext::set_processor(&p0);
  ASSERT_EQ(ThreadContext::get_worker_id(), 3);

  ThreadContext::set_processor(nullptr);
  EXPECT_EQ(ThreadContext::get_processor(), nullptr);
  EXPECT_EQ(ThreadContext::get_worker_id(), -1);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
