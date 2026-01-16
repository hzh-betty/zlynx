/**
 * @file concurrent_alloc_test.cc
 * @brief 并发分配集成测试
 */

#include "zmalloc.h"
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace zmalloc {
namespace {

class ConcurrentAllocTest : public ::testing::Test {};

// 多线程同时分配释放
TEST_F(ConcurrentAllocTest, MultiThreadBasic) {
  constexpr int kThreadCount = 4;
  constexpr int kAllocsPerThread = 1000;
  constexpr size_t kAllocSize = 64;

  std::atomic<int> success_count{0};

  auto worker = [&]() {
    std::vector<void *> ptrs;
    for (int i = 0; i < kAllocsPerThread; ++i) {
      void *ptr = zmalloc(kAllocSize);
      if (ptr != nullptr) {
        ptrs.push_back(ptr);
      }
    }
    for (void *ptr : ptrs) {
      zfree(ptr);
    }
    success_count.fetch_add(static_cast<int>(ptrs.size()));
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), kThreadCount * kAllocsPerThread);
}

// 多线程不同大小分配
TEST_F(ConcurrentAllocTest, MultiThreadVariousSizes) {
  constexpr int kThreadCount = 4;
  constexpr int kAllocsPerThread = 500;

  auto worker = [](size_t base_size) {
    std::vector<void *> ptrs;
    for (int i = 0; i < kAllocsPerThread; ++i) {
      size_t size = base_size + (i % 128);
      void *ptr = zmalloc(size);
      if (ptr != nullptr) {
        ptrs.push_back(ptr);
      }
    }
    for (void *ptr : ptrs) {
      zfree(ptr);
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(worker, 8);
  threads.emplace_back(worker, 256);
  threads.emplace_back(worker, 1024);
  threads.emplace_back(worker, 8192);

  for (auto &t : threads) {
    t.join();
  }
}

// 生产者消费者模式
TEST_F(ConcurrentAllocTest, ProducerConsumer) {
  constexpr int kItemCount = 1000;
  std::vector<void *> shared_ptrs(kItemCount, nullptr);
  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};

  // 生产者
  auto producer = [&]() {
    for (int i = 0; i < kItemCount; ++i) {
      shared_ptrs[i] = zmalloc(64);
      produced.fetch_add(1);
    }
  };

  // 消费者
  auto consumer = [&]() {
    while (consumed.load() < kItemCount) {
      int idx = consumed.load();
      if (idx < produced.load()) {
        if (consumed.compare_exchange_weak(idx, idx + 1)) {
          if (shared_ptrs[idx] != nullptr) {
            zfree(shared_ptrs[idx]);
            shared_ptrs[idx] = nullptr;
          }
        }
      }
      std::this_thread::yield();
    }
  };

  std::thread prod_thread(producer);
  std::thread cons_thread(consumer);

  prod_thread.join();
  cons_thread.join();

  EXPECT_EQ(produced.load(), kItemCount);
  EXPECT_EQ(consumed.load(), kItemCount);
}

// 大对象多线程分配
TEST_F(ConcurrentAllocTest, MultiThreadLargeAlloc) {
  constexpr int kThreadCount = 2;
  constexpr int kAllocsPerThread = 10;

  auto worker = []() {
    std::vector<void *> ptrs;
    for (int i = 0; i < kAllocsPerThread; ++i) {
      void *ptr = zmalloc(512 * 1024); // > MAX_BYTES
      if (ptr != nullptr) {
        ptrs.push_back(ptr);
      }
    }
    for (void *ptr : ptrs) {
      zfree(ptr);
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }
}

// 压力测试
TEST_F(ConcurrentAllocTest, StressTest) {
  constexpr int kThreadCount = 8;
  constexpr int kIterations = 100;

  auto worker = [](unsigned int seed) {
    for (int iter = 0; iter < kIterations; ++iter) {
      std::vector<void *> ptrs;
      for (int i = 0; i < 100; ++i) {
        size_t size = 8 + (seed % 1000);
        seed = seed * 1103515245 + 12345; // 简单的线性同余随机
        ptrs.push_back(zmalloc(size));
      }
      for (void *ptr : ptrs) {
        zfree(ptr);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, static_cast<unsigned int>(i * 12345));
  }
  for (auto &t : threads) {
    t.join();
  }
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
