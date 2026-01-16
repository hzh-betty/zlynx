/**
 * @file concurrent_alloc_test.cc
 * @brief 并发分配集成测试
 */

#include "zmalloc.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
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

// 高并发小对象分配
TEST_F(ConcurrentAllocTest, HighConcurrencySmallAlloc) {
  constexpr int kThreadCount = 16;
  constexpr int kAllocsPerThread = 500;

  std::atomic<int> success{0};

  auto worker = [&success]() {
    for (int i = 0; i < kAllocsPerThread; ++i) {
      void *ptr = zmalloc(16);
      if (ptr != nullptr) {
        success.fetch_add(1);
        zfree(ptr);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success.load(), kThreadCount * kAllocsPerThread);
}

// 交错分配释放
TEST_F(ConcurrentAllocTest, InterleavedAllocFree) {
  constexpr int kThreadCount = 4;
  constexpr int kIterations = 200;

  auto worker = []() {
    for (int i = 0; i < kIterations; ++i) {
      void *p1 = zmalloc(64);
      void *p2 = zmalloc(128);
      zfree(p1);
      void *p3 = zmalloc(256);
      zfree(p2);
      zfree(p3);
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

// 不同线程分配不同大小
TEST_F(ConcurrentAllocTest, DifferentSizesPerThread) {
  constexpr int kThreadCount = 8;

  auto worker = [](size_t size) {
    std::vector<void *> ptrs;
    for (int i = 0; i < 100; ++i) {
      ptrs.push_back(zmalloc(size));
    }
    for (void *ptr : ptrs) {
      zfree(ptr);
    }
  };

  std::vector<std::thread> threads;
  size_t sizes[] = {8, 32, 64, 128, 256, 512, 1024, 4096};
  for (size_t s : sizes) {
    threads.emplace_back(worker, s);
  }
  for (auto &t : threads) {
    t.join();
  }
}

// 混合大小并发分配
TEST_F(ConcurrentAllocTest, MixedSizesConcurrent) {
  constexpr int kThreadCount = 4;
  constexpr int kIterations = 50;

  auto worker = [](int thread_id) {
    unsigned int seed = static_cast<unsigned int>(thread_id * 7919);
    for (int i = 0; i < kIterations; ++i) {
      std::vector<std::pair<void *, size_t>> allocations;
      for (int j = 0; j < 20; ++j) {
        seed = seed * 1103515245 + 12345;
        size_t size = 8 + (seed % 4000);
        void *ptr = zmalloc(size);
        allocations.push_back({ptr, size});
      }
      for (auto &a : allocations) {
        zfree(a.first);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }
}

// 长时间运行测试
TEST_F(ConcurrentAllocTest, LongRunning) {
  constexpr int kThreadCount = 2;
  constexpr int kIterations = 500;

  auto worker = []() {
    for (int i = 0; i < kIterations; ++i) {
      std::vector<void *> ptrs;
      for (int j = 0; j < 50; ++j) {
        ptrs.push_back(zmalloc(64 + j));
      }
      for (void *ptr : ptrs) {
        zfree(ptr);
      }
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

// 批量分配后批量释放
TEST_F(ConcurrentAllocTest, BatchAllocBatchFree) {
  constexpr int kThreadCount = 4;

  auto worker = []() {
    for (int round = 0; round < 10; ++round) {
      std::vector<void *> ptrs;
      // 批量分配
      for (int i = 0; i < 200; ++i) {
        ptrs.push_back(zmalloc(100));
      }
      // 批量释放
      for (void *ptr : ptrs) {
        zfree(ptr);
      }
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

// 大对象并发分配
TEST_F(ConcurrentAllocTest, LargeObjectConcurrent) {
  constexpr int kThreadCount = 4;

  auto worker = []() {
    for (int i = 0; i < 20; ++i) {
      void *ptr = zmalloc(100 * 1024); // 100KB
      EXPECT_NE(ptr, nullptr);
      std::memset(ptr, 0xAA, 100 * 1024);
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

// 超大对象并发分配 (> MAX_BYTES)
TEST_F(ConcurrentAllocTest, HugeObjectConcurrent) {
  constexpr int kThreadCount = 2;

  auto worker = []() {
    for (int i = 0; i < 5; ++i) {
      void *ptr = zmalloc(512 * 1024); // 512KB > MAX_BYTES
      EXPECT_NE(ptr, nullptr);
      std::memset(ptr, 0xBB, 512 * 1024);
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

// 快速分配释放
TEST_F(ConcurrentAllocTest, RapidAllocFree) {
  constexpr int kThreadCount = 8;
  constexpr int kIterations = 1000;

  auto worker = []() {
    for (int i = 0; i < kIterations; ++i) {
      void *ptr = zmalloc(32);
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

// 数据完整性并发测试
TEST_F(ConcurrentAllocTest, DataIntegrityConcurrent) {
  constexpr int kThreadCount = 4;

  auto worker = [](int thread_id) {
    for (int i = 0; i < 50; ++i) {
      size_t size = 256;
      unsigned char *ptr = static_cast<unsigned char *>(zmalloc(size));
      EXPECT_NE(ptr, nullptr);

      unsigned char pattern = static_cast<unsigned char>(thread_id * 10 + i);
      std::memset(ptr, pattern, size);

      for (size_t j = 0; j < size; ++j) {
        EXPECT_EQ(ptr[j], pattern);
      }

      zfree(ptr);
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }
}

// 多生产者单消费者
TEST_F(ConcurrentAllocTest, MultiProducerSingleConsumer) {
  constexpr int kProducers = 4;
  constexpr int kItemsPerProducer = 100;

  std::vector<void *> shared_buffer(kProducers * kItemsPerProducer, nullptr);
  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};

  auto producer = [&shared_buffer, &produced](int start_idx) {
    for (int i = 0; i < kItemsPerProducer; ++i) {
      shared_buffer[start_idx + i] = zmalloc(64);
      produced.fetch_add(1);
    }
  };

  auto consumer = [&shared_buffer, &produced, &consumed]() {
    int total = kProducers * kItemsPerProducer;
    while (consumed.load() < total) {
      int idx = consumed.load();
      if (idx < produced.load() && shared_buffer[idx] != nullptr) {
        if (consumed.compare_exchange_weak(idx, idx + 1)) {
          zfree(shared_buffer[idx]);
          shared_buffer[idx] = nullptr;
        }
      }
      std::this_thread::yield();
    }
  };

  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; ++i) {
    producers.emplace_back(producer, i * kItemsPerProducer);
  }
  std::thread cons_thread(consumer);

  for (auto &t : producers) {
    t.join();
  }
  cons_thread.join();

  EXPECT_EQ(consumed.load(), kProducers * kItemsPerProducer);
}

// 边界大小并发分配
TEST_F(ConcurrentAllocTest, BoundarySizesConcurrent) {
  constexpr int kIterations = 50;

  auto worker = [](size_t size) {
    for (int i = 0; i < kIterations; ++i) {
      void *ptr = zmalloc(size);
      EXPECT_NE(ptr, nullptr);
      zfree(ptr);
    }
  };

  std::vector<std::thread> threads;
  size_t boundary_sizes[] = {128,       129,           1024,
                             1025,      8 * 1024,      8 * 1024 + 1,
                             64 * 1024, 64 * 1024 + 1, 256 * 1024};
  for (size_t s : boundary_sizes) {
    threads.emplace_back(worker, s);
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
