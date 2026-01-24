/**
 * @file work_stealing_thread_pool_submit_test.cc
 * @brief WorkStealingThreadPool::submit 单元测试
 */

#include "work_stealing_thread_pool.h"
#include "work_stealing_queue.h"
#include "zcoroutine_logger.h"

#include <atomic>
#include <gtest/gtest.h>

using namespace zcoroutine;

class WorkStealingThreadPoolSubmitTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  }
};

static size_t drain_one_task(WorkStealingQueue *q) {
  if (!q) {
    return 0;
  }
  Task out[1];
  const size_t n = q->pop_batch(out, 1);
  if (n == 1 && out[0].is_valid() && out[0].callback) {
    out[0].callback();
    out[0].reset();
  }
  return n;
}

TEST_F(WorkStealingThreadPoolSubmitTest, SubmitWithHintGoesToThatProcessor) {
  WorkStealingThreadPool pool(2, "submit-test");

  std::atomic<int> counter{0};
  Task task([&counter]() { counter.fetch_add(1); });

  Processor *p0 = pool.processor(0);
  ASSERT_NE(p0, nullptr);

  ASSERT_TRUE(pool.submit(std::move(task), p0));

  // 只应在 p0 的 runq 中出现。
  EXPECT_EQ(drain_one_task(&p0->run_queue), 1u);
  EXPECT_EQ(counter.load(), 1);

  Processor *p1 = pool.processor(1);
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(p1->run_queue.approx_size(), 0u);
}

TEST_F(WorkStealingThreadPoolSubmitTest, SubmitWithoutHintEnqueuesSomewhere) {
  WorkStealingThreadPool pool(2, "submit-test");

  std::atomic<int> counter{0};
  Task task([&counter]() { counter.fetch_add(1); });

  ASSERT_TRUE(pool.submit(std::move(task), nullptr));

  size_t drained = 0;
  for (int i = 0; i < pool.thread_count(); ++i) {
    Processor *p = pool.processor(i);
    ASSERT_NE(p, nullptr);
    drained += drain_one_task(&p->run_queue);
  }

  EXPECT_EQ(drained, 1u);
  EXPECT_EQ(counter.load(), 1);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
