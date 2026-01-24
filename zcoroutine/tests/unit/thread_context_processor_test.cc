/**
 * @file thread_context_processor_test.cc
 * @brief ThreadContext Processor 绑定相关单元测试
 */

#include "processor.h"
#include "thread_context.h"
#include "work_stealing_queue.h"
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
    ThreadContext::set_work_queue(nullptr);
    ThreadContext::set_worker_id(-1);
    ThreadContext::set_scheduler(nullptr);
    ThreadContext::set_main_fiber(nullptr);
    ThreadContext::set_scheduler_fiber(nullptr);
    ThreadContext::set_current_fiber(nullptr);
  }
};

TEST_F(ThreadContextProcessorTest, SetProcessorDerivesWorkQueueAndWorkerId) {
  Processor p0(7);

  ThreadContext::set_processor(&p0);

  EXPECT_EQ(ThreadContext::get_processor(), &p0);
  EXPECT_EQ(ThreadContext::get_worker_id(), 7);
  EXPECT_EQ(ThreadContext::get_work_queue(), &p0.run_queue);
}

TEST_F(ThreadContextProcessorTest, SetWorkQueueClearsProcessorToAvoidAmbiguity) {
  Processor p0(0);
  ThreadContext::set_processor(&p0);
  ASSERT_EQ(ThreadContext::get_work_queue(), &p0.run_queue);

  WorkStealingQueue injected;
  ThreadContext::set_work_queue(&injected);

  EXPECT_EQ(ThreadContext::get_processor(), nullptr);
  EXPECT_EQ(ThreadContext::get_work_queue(), &injected);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
