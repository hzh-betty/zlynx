/**
 * @file work_stealing_queue_bitmap_integration_test.cc
 * @brief WorkStealingQueue + StealableQueueBitmap 集成测试
 */

#include "stealable_queue_bitmap.h"
#include "work_stealing_queue.h"
#include "zcoroutine_logger.h"

#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace zcoroutine;

class WorkStealingQueueBitmapIntegrationTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  }
};

TEST_F(WorkStealingQueueBitmapIntegrationTest,
       PublishAndUnpublishByStealBatch) {
  StealableQueueBitmap bitmap(2);

  WorkStealingQueue q;
  q.bind_bitmap(&bitmap, 0, /*high=*/4, /*low=*/2);

  for (int i = 0; i < 5; ++i) {
    q.push(Task([]() {}));
  }
  ASSERT_EQ(q.approx_size(), 5u);

  // 仅 worker0 可窃取，self=1 应能找到 victim=0
  EXPECT_EQ(bitmap.find_victim(1), 0);

  Task out[4];
  EXPECT_EQ(q.steal_batch(out, 4), 4u);
  EXPECT_EQ(q.approx_size(), 1u);

  // size=1 < low(2) 触发撤销
  EXPECT_EQ(bitmap.find_victim(1), -1);
}

TEST_F(WorkStealingQueueBitmapIntegrationTest,
       SwapIfEmptyUpdatesBitmapForBothQueues) {
  StealableQueueBitmap bitmap(2);

  WorkStealingQueue q0;
  WorkStealingQueue q1;
  q0.bind_bitmap(&bitmap, 0, /*high=*/3, /*low=*/1);
  q1.bind_bitmap(&bitmap, 1, /*high=*/3, /*low=*/1);

  for (int i = 0; i < 4; ++i) {
    q1.push(Task([]() {}));
  }
  ASSERT_EQ(q1.approx_size(), 4u);

  // 只有 q1 超过高水位，self=0 -> start=1，应命中1
  EXPECT_EQ(bitmap.find_victim(0), 1);

  ASSERT_TRUE(q0.empty());
  const size_t moved = q0.swap_if_empty(q1);
  ASSERT_EQ(moved, 4u);
  EXPECT_EQ(q0.approx_size(), 4u);
  EXPECT_TRUE(q1.empty());

  // swap 后：q0 变得可窃取；q1 变空，应撤销
  EXPECT_EQ(bitmap.find_victim(1), 0);
  EXPECT_EQ(bitmap.find_victim(0), -1);
}

TEST_F(WorkStealingQueueBitmapIntegrationTest,
       BitmapGuidedStealConsumesAllExactlyOnce) {
  constexpr int kTaskCount = 2000;

  StealableQueueBitmap bitmap(2);
  WorkStealingQueue victim;
  WorkStealingQueue thief;
  // low=1：避免在队列仍有少量任务时提前撤销发布，导致纯 bitmap
  // 引导的窃取循环卡住。
  victim.bind_bitmap(&bitmap, 1, /*high=*/32, /*low=*/1);
  thief.bind_bitmap(&bitmap, 0, /*high=*/32, /*low=*/1);

  auto seen = std::make_unique<std::atomic<int>[]>(kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    seen[i].store(0, std::memory_order_relaxed);
  }

  std::atomic<int> processed{0};

  for (int id = 0; id < kTaskCount; ++id) {
    victim.push(Task([&, id]() {
      seen[id].fetch_add(1, std::memory_order_relaxed);
      processed.fetch_add(1, std::memory_order_relaxed);
    }));
  }
  ASSERT_EQ(victim.approx_size(), static_cast<size_t>(kTaskCount));
  ASSERT_EQ(bitmap.find_victim(0), 1);

  std::thread stealer([&]() {
    Task buf[16];
    while (processed.load(std::memory_order_relaxed) < kTaskCount) {
      const int vid = bitmap.find_victim(0);
      if (vid != 1) {
        std::this_thread::yield();
        continue;
      }

      const size_t n = victim.steal_batch(buf, 16);
      if (n == 0) {
        std::this_thread::yield();
        continue;
      }

      for (size_t i = 0; i < n; ++i) {
        if (buf[i].callback) {
          buf[i].callback();
        }
        buf[i].reset();
      }
    }
  });

  stealer.join();

  EXPECT_EQ(processed.load(std::memory_order_relaxed), kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1);
  }

  EXPECT_TRUE(victim.empty());
  EXPECT_EQ(bitmap.find_victim(0), -1);
}

TEST_F(WorkStealingQueueBitmapIntegrationTest, WatermarkBoundariesAreStrict) {
  StealableQueueBitmap bitmap(2);

  WorkStealingQueue q;
  q.bind_bitmap(&bitmap, 0, /*high=*/4, /*low=*/2);

  // new_size == high：不发布（严格大于才发布）
  for (int i = 0; i < 4; ++i) {
    q.push(Task([]() {}));
  }
  ASSERT_EQ(q.approx_size(), 4u);
  EXPECT_EQ(bitmap.find_victim(1), -1);

  // new_size > high：发布
  q.push(Task([]() {}));
  ASSERT_EQ(q.approx_size(), 5u);
  EXPECT_EQ(bitmap.find_victim(1), 0);

  // new_size == low：不撤销（严格小于才撤销）
  Task out[3];
  ASSERT_EQ(q.steal_batch(out, 3), 3u);
  EXPECT_EQ(q.approx_size(), 2u);
  EXPECT_EQ(bitmap.find_victim(1), 0);

  // new_size < low：撤销
  Task out2[1];
  ASSERT_EQ(q.steal_batch(out2, 1), 1u);
  EXPECT_EQ(q.approx_size(), 1u);
  EXPECT_EQ(bitmap.find_victim(1), -1);
}

TEST_F(WorkStealingQueueBitmapIntegrationTest,
       PublishAndUnpublishByOwnerPopBatch) {
  StealableQueueBitmap bitmap(2);

  WorkStealingQueue q;
  q.bind_bitmap(&bitmap, 0, /*high=*/3, /*low=*/2);

  for (int i = 0; i < 4; ++i) {
    q.push(Task([]() {}));
  }
  ASSERT_EQ(q.approx_size(), 4u);
  EXPECT_EQ(bitmap.find_victim(1), 0);

  // owner pop 导致 new_size < low，应撤销
  Task out[3];
  ASSERT_EQ(q.pop_batch(out, 3), 3u);
  EXPECT_EQ(q.approx_size(), 1u);
  EXPECT_EQ(bitmap.find_victim(1), -1);
}

TEST_F(WorkStealingQueueBitmapIntegrationTest,
       BitmapGuidedStealFromMultipleVictimsConsumesAllExactlyOnce) {
  constexpr int kTaskCount = 3000;

  StealableQueueBitmap bitmap(4);
  WorkStealingQueue q0;
  WorkStealingQueue q1;
  WorkStealingQueue q2;
  WorkStealingQueue q3;
  q0.bind_bitmap(&bitmap, 0, /*high=*/32, /*low=*/1);
  q1.bind_bitmap(&bitmap, 1, /*high=*/32, /*low=*/1);
  q2.bind_bitmap(&bitmap, 2, /*high=*/32, /*low=*/1);
  q3.bind_bitmap(&bitmap, 3, /*high=*/32, /*low=*/1);

  auto seen = std::make_unique<std::atomic<int>[]>(kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    seen[i].store(0, std::memory_order_relaxed);
  }
  std::atomic<int> processed{0};

  // 将任务分散到 3 个 victim，上报位图后由 worker0 纯靠 bitmap 引导完成窃取。
  for (int id = 0; id < kTaskCount; ++id) {
    WorkStealingQueue *dst = nullptr;
    switch (id % 3) {
    case 0:
      dst = &q1;
      break;
    case 1:
      dst = &q2;
      break;
    default:
      dst = &q3;
      break;
    }
    dst->push(Task([&, id]() {
      seen[id].fetch_add(1, std::memory_order_relaxed);
      processed.fetch_add(1, std::memory_order_relaxed);
    }));
  }

  ASSERT_EQ(processed.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(bitmap.find_victim(0), 1);

  WorkStealingQueue *queues[4] = {&q0, &q1, &q2, &q3};

  std::thread stealer([&]() {
    Task buf[32];
    while (processed.load(std::memory_order_relaxed) < kTaskCount) {
      const int vid = bitmap.find_victim(0);
      if (vid < 1 || vid > 3) {
        std::this_thread::yield();
        continue;
      }

      const size_t n = queues[vid]->steal_batch(buf, 32);
      if (n == 0) {
        std::this_thread::yield();
        continue;
      }
      for (size_t i = 0; i < n; ++i) {
        if (buf[i].callback) {
          buf[i].callback();
        }
        buf[i].reset();
      }
    }
  });

  stealer.join();

  EXPECT_EQ(processed.load(std::memory_order_relaxed), kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1);
  }

  EXPECT_TRUE(q1.empty());
  EXPECT_TRUE(q2.empty());
  EXPECT_TRUE(q3.empty());
  EXPECT_EQ(bitmap.find_victim(0), -1);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
