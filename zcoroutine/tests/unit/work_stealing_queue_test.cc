/**
 * @file work_stealing_queue_test.cc
 * @brief WorkStealingQueue 单元测试
 */

#include "stealable_queue_bitmap.h"
#include "work_stealing_queue.h"
#include "zcoroutine_logger.h"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace zcoroutine;

namespace {

Task make_id_task(int id, std::vector<int> *sink, std::mutex *sink_mutex) {
  return Task([id, sink, sink_mutex]() {
    std::lock_guard<std::mutex> lock(*sink_mutex);
    sink->push_back(id);
  });
}

} // namespace

class WorkStealingQueueTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  }

  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(WorkStealingQueueTest, PushPopBatchLifoOrder) {
  WorkStealingQueue q;

  std::vector<int> executed;
  std::mutex executed_mutex;

  for (int i = 0; i < 8; ++i) {
    q.push(make_id_task(i, &executed, &executed_mutex));
  }

  Task out[8];
  const size_t n = q.pop_batch(out, 8);
  ASSERT_EQ(n, 8u);

  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out[i].is_valid());
    ASSERT_TRUE(static_cast<bool>(out[i].callback));
    out[i].callback();
    out[i].reset();
  }

  // owner pop_back: LIFO
  ASSERT_EQ(executed.size(), 8u);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(executed[static_cast<size_t>(i)], 7 - i);
  }

  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.approx_size(), 0u);
}

TEST_F(WorkStealingQueueTest, StealBatchFifoOrder) {
  WorkStealingQueue q;

  std::vector<int> executed;
  std::mutex executed_mutex;

  for (int i = 0; i < 10; ++i) {
    q.push(make_id_task(i, &executed, &executed_mutex));
  }

  Task out[4];
  const size_t n = q.steal_batch(out, 4);
  ASSERT_EQ(n, 4u);

  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out[i].is_valid());
    out[i].callback();
    out[i].reset();
  }

  // thief pop_front: FIFO
  ASSERT_EQ(executed.size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(executed[static_cast<size_t>(i)], i);
  }

  EXPECT_EQ(q.approx_size(), 6u);
}

TEST_F(WorkStealingQueueTest, ApproxSizeTracksPushPopSteal) {
  WorkStealingQueue q;

  std::vector<int> executed;
  std::mutex executed_mutex;

  EXPECT_EQ(q.approx_size(), 0u);

  for (int i = 0; i < 5; ++i) {
    q.push(make_id_task(i, &executed, &executed_mutex));
  }
  EXPECT_EQ(q.approx_size(), 5u);

  Task out[2];
  EXPECT_EQ(q.pop_batch(out, 2), 2u);
  EXPECT_EQ(q.approx_size(), 3u);

  EXPECT_EQ(q.steal_batch(out, 2), 2u);
  EXPECT_EQ(q.approx_size(), 1u);

  EXPECT_EQ(q.pop_batch(out, 2), 1u);
  EXPECT_EQ(q.approx_size(), 0u);
  EXPECT_TRUE(q.empty());
}

TEST_F(WorkStealingQueueTest, PopAndStealWithZeroMaxCountReturnZero) {
  WorkStealingQueue q;

  q.push(Task([]() {}));
  EXPECT_EQ(q.approx_size(), 1u);

  Task out[1];
  EXPECT_EQ(q.pop_batch(out, 0), 0u);
  EXPECT_EQ(q.steal_batch(out, 0), 0u);
  EXPECT_EQ(q.approx_size(), 1u);
}

TEST_F(WorkStealingQueueTest, WaitPopBatchFastPathWhenNotEmpty) {
  WorkStealingQueue q;
  std::atomic<int> value{0};

  q.push(Task([&]() { value.store(1, std::memory_order_relaxed); }));
  q.push(Task([&]() { value.store(2, std::memory_order_relaxed); }));

  Task out[1];
  auto start = std::chrono::steady_clock::now();
  const size_t n = q.wait_pop_batch(out, 1, 1000);
  auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_EQ(n, 1u);
  ASSERT_TRUE(out[0].is_valid());
  out[0].callback();

  // wait_pop_batch 在队列非空时应直接走 pop_batch，且 owner 语义为 LIFO。
  EXPECT_EQ(value.load(std::memory_order_relaxed), 2);
  EXPECT_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      200);
}

TEST_F(WorkStealingQueueTest, WaitPopBatchTimeoutReturnsZero) {
  WorkStealingQueue q;
  Task out[1];

  auto start = std::chrono::steady_clock::now();
  const size_t n = q.wait_pop_batch(out, 1, 30);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(n, 0u);
  // 不做严格下限断言（避免 CI/系统抖动），只要别卡死即可。
  EXPECT_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      500);
}

TEST_F(WorkStealingQueueTest, WaitPopBatchWakesOnPush) {
  WorkStealingQueue q;
  std::atomic<int> value{0};

  std::thread waiter([&]() {
    Task out[1];
    const size_t n = q.wait_pop_batch(out, 1, 1000);
    ASSERT_EQ(n, 1u);
    ASSERT_TRUE(out[0].is_valid());
    ASSERT_TRUE(static_cast<bool>(out[0].callback));
    out[0].callback();
  });

  // 确保 waiter 进入 wait（尽量减少竞态）
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  q.push(Task([&]() { value.store(123, std::memory_order_relaxed); }));

  waiter.join();
  EXPECT_EQ(value.load(std::memory_order_relaxed), 123);
}

TEST_F(WorkStealingQueueTest, StopUnblocksWaiters) {
  WorkStealingQueue q;
  std::atomic<bool> finished{false};

  std::thread waiter([&]() {
    Task out[1];
    const size_t n = q.wait_pop_batch(out, 1, 0); // 永久等待
    EXPECT_EQ(n, 0u);
    finished.store(true, std::memory_order_relaxed);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  q.stop();

  waiter.join();
  EXPECT_TRUE(finished.load(std::memory_order_relaxed));
}

TEST_F(WorkStealingQueueTest, StopDoesNotDiscardExistingTasks) {
  WorkStealingQueue q;
  std::atomic<int> value{0};

  q.push(Task([&]() { value.fetch_add(1, std::memory_order_relaxed); }));
  q.stop();

  Task out[1];
  const size_t n = q.wait_pop_batch(out, 1, 0);
  ASSERT_EQ(n, 1u);
  out[0].callback();

  EXPECT_EQ(value.load(std::memory_order_relaxed), 1);
}

TEST_F(WorkStealingQueueTest, NotifyOneDoesNotCauseEmptyQueueToReturn) {
  WorkStealingQueue q;
  std::atomic<int> state{0};

  std::thread waiter([&]() {
    state.store(1, std::memory_order_release);
    Task out[1];
    const size_t n = q.wait_pop_batch(out, 1, 0);
    if (n == 0) {
      state.store(-1, std::memory_order_release);
      return;
    }
    if (out[0].callback) {
      out[0].callback();
    }
  });

  while (state.load(std::memory_order_acquire) == 0) {
    std::this_thread::yield();
  }

  // 空队列下 notify_one 只会唤醒重检谓词，不应导致 wait_pop_batch 直接返回。
  q.notify_one();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(state.load(std::memory_order_acquire), 1);

  q.push(Task([&]() { state.store(2, std::memory_order_release); }));
  waiter.join();

  EXPECT_EQ(state.load(std::memory_order_acquire), 2);
}

TEST_F(WorkStealingQueueTest, SwapIfEmptyStealsDequeInO1) {
  WorkStealingQueue self;
  WorkStealingQueue victim;

  std::vector<int> executed;
  std::mutex executed_mutex;

  for (int i = 0; i < 5; ++i) {
    victim.push(make_id_task(i, &executed, &executed_mutex));
  }
  ASSERT_TRUE(self.empty());
  ASSERT_EQ(victim.approx_size(), 5u);

  const size_t moved = self.swap_if_empty(victim);
  EXPECT_EQ(moved, 5u);
  EXPECT_EQ(self.approx_size(), 5u);
  EXPECT_TRUE(victim.empty());

  Task out[5];
  const size_t n = self.pop_batch(out, 5);
  ASSERT_EQ(n, 5u);
  for (size_t i = 0; i < n; ++i) {
    out[i].callback();
    out[i].reset();
  }

  // self 作为 owner，pop_back：LIFO
  ASSERT_EQ(executed.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(executed[static_cast<size_t>(i)], 4 - i);
  }

  EXPECT_TRUE(self.empty());
  EXPECT_EQ(self.swap_if_empty(victim), 0u);
}

TEST_F(WorkStealingQueueTest, SwapIfEmptyNoOpOnSelfOrNonEmptyOrEmptyVictim) {
  WorkStealingQueue a;
  WorkStealingQueue b;

  a.push(Task([]() {}));
  b.push(Task([]() {}));

  EXPECT_EQ(a.swap_if_empty(a), 0u);
  EXPECT_EQ(a.swap_if_empty(b), 0u); // a 非空

  Task out[1];
  EXPECT_EQ(a.pop_batch(out, 1), 1u);
  EXPECT_TRUE(a.empty());

  // b 非空，a 为空，此时可以交换
  EXPECT_EQ(a.swap_if_empty(b), 1u);
  EXPECT_EQ(a.approx_size(), 1u);
  EXPECT_TRUE(b.empty());

  // victim 为空，无交换
  WorkStealingQueue c;
  EXPECT_TRUE(c.empty());
  EXPECT_EQ(b.swap_if_empty(c), 0u);
}

TEST_F(WorkStealingQueueTest, BitmapPublishAndUnpublishByWatermarks) {
  StealableQueueBitmap bitmap(2);
  WorkStealingQueue q;

  // H=4, L=2: size>4 发布；size<2 撤销
  q.bind_bitmap(&bitmap, 0, 4, 2);

  for (int i = 0; i < 5; ++i) {
    q.push(Task([]() {}));
  }
  EXPECT_EQ(q.approx_size(), 5u);
  EXPECT_EQ(bitmap.find_victim(1), 0);

  Task out[4];
  EXPECT_EQ(q.pop_batch(out, 4), 4u); // size 从 5 -> 1，触发撤销（1 < 2）
  EXPECT_EQ(q.approx_size(), 1u);
  EXPECT_EQ(bitmap.find_victim(1), -1);
}

TEST_F(WorkStealingQueueTest,
       BitmapBindAfterQueueAlreadyLargePublishesImmediately) {
  StealableQueueBitmap bitmap(2);
  WorkStealingQueue q;

  for (int i = 0; i < 6; ++i) {
    q.push(Task([]() {}));
  }
  ASSERT_EQ(q.approx_size(), 6u);

  q.bind_bitmap(&bitmap, 0, 4, 2);
  EXPECT_EQ(bitmap.find_victim(1), 0);
}

TEST_F(WorkStealingQueueTest, BitmapDisabledOnInvalidParamsDoesNotPublish) {
  StealableQueueBitmap bitmap(2);
  WorkStealingQueue q;

  // low >= high：按实现应禁用
  q.bind_bitmap(&bitmap, 0, 2, 2);
  for (int i = 0; i < 10; ++i) {
    q.push(Task([]() {}));
  }
  EXPECT_EQ(bitmap.find_victim(1), -1);
}

TEST_F(WorkStealingQueueTest, ConcurrentOwnerAndThiefConsumeAllExactlyOnce) {
  WorkStealingQueue q;

  constexpr int kTaskCount = 2000;
  auto seen = std::make_unique<std::atomic<int>[]>(kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    seen[i].store(0, std::memory_order_relaxed);
  }

  std::atomic<int> processed{0};
  std::atomic<bool> producer_done{false};

  auto consume_task = [&](Task &t) {
    if (!t.is_valid() || !t.callback) {
      return;
    }
    t.callback();
    t.reset();
  };

  std::thread producer([&]() {
    for (int id = 0; id < kTaskCount; ++id) {
      q.push(Task([&, id]() {
        seen[id].fetch_add(1, std::memory_order_relaxed);
        processed.fetch_add(1, std::memory_order_relaxed);
      }));
      if ((id % 200) == 0) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread owner([&]() {
    Task buf[16];
    while (true) {
      if (processed.load(std::memory_order_relaxed) >= kTaskCount) {
        break;
      }

      size_t n = q.pop_batch(buf, 16);
      if (n == 0) {
        n = q.wait_pop_batch(buf, 16, 10);
      }
      if (n == 0) {
        if (producer_done.load(std::memory_order_acquire) && q.empty()) {
          break;
        }
        continue;
      }
      for (size_t i = 0; i < n; ++i) {
        consume_task(buf[i]);
      }
    }
  });

  std::thread thief([&]() {
    Task buf[8];
    while (true) {
      if (processed.load(std::memory_order_relaxed) >= kTaskCount) {
        break;
      }

      const size_t n = q.steal_batch(buf, 8);
      if (n == 0) {
        if (producer_done.load(std::memory_order_acquire) && q.empty()) {
          break;
        }
        std::this_thread::yield();
        continue;
      }
      for (size_t i = 0; i < n; ++i) {
        consume_task(buf[i]);
      }
    }
  });

  producer.join();
  owner.join();
  thief.join();

  EXPECT_EQ(processed.load(std::memory_order_relaxed), kTaskCount);
  for (int i = 0; i < kTaskCount; ++i) {
    EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1);
  }
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
