/**
 * @file benchmark.cc
 * @brief zmalloc 性能基准测试
 */

#include "zmalloc.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using Milliseconds = std::chrono::milliseconds;

namespace {

void benchmark_single_thread(size_t alloc_size, size_t num_allocs,
                             size_t rounds) {
  std::cout << "Single-thread benchmark: size=" << alloc_size
            << ", allocs=" << num_allocs << ", rounds=" << rounds << std::endl;

  // zmalloc
  auto start = Clock::now();
  for (size_t r = 0; r < rounds; ++r) {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(zmalloc::zmalloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      zmalloc::zfree(ptr);
    }
  }
  auto end = Clock::now();
  auto zmalloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  // malloc
  start = Clock::now();
  for (size_t r = 0; r < rounds; ++r) {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(malloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      free(ptr);
    }
  }
  end = Clock::now();
  auto malloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  std::cout << "  zmalloc: " << zmalloc_time << " ms" << std::endl;
  std::cout << "  malloc:  " << malloc_time << " ms" << std::endl;
  std::cout << "  ratio:   " << static_cast<double>(malloc_time) / zmalloc_time
            << "x" << std::endl;
  std::cout << std::endl;
}

void benchmark_multi_thread(size_t alloc_size, size_t num_allocs,
                            int thread_count) {
  std::cout << "Multi-thread benchmark: size=" << alloc_size
            << ", allocs=" << num_allocs << ", threads=" << thread_count
            << std::endl;

  auto zmalloc_worker = [=]() {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(zmalloc::zmalloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      zmalloc::zfree(ptr);
    }
  };

  auto malloc_worker = [=]() {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(malloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      free(ptr);
    }
  };

  // zmalloc
  auto start = Clock::now();
  {
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
      threads.emplace_back(zmalloc_worker);
    }
    for (auto &t : threads) {
      t.join();
    }
  }
  auto end = Clock::now();
  auto zmalloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  // malloc
  start = Clock::now();
  {
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
      threads.emplace_back(malloc_worker);
    }
    for (auto &t : threads) {
      t.join();
    }
  }
  end = Clock::now();
  auto malloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  std::cout << "  zmalloc: " << zmalloc_time << " ms" << std::endl;
  std::cout << "  malloc:  " << malloc_time << " ms" << std::endl;
  std::cout << "  ratio:   " << static_cast<double>(malloc_time) / zmalloc_time
            << "x" << std::endl;
  std::cout << std::endl;
}

void benchmark_random_single_thread(size_t min_size, size_t max_size,
                                    size_t num_allocs, size_t rounds) {
  std::cout << "Single-thread random-size benchmark: range=[" << min_size << ","
            << max_size << "]" << ", allocs=" << num_allocs
            << ", rounds=" << rounds << std::endl;

  std::vector<size_t> sizes;
  sizes.reserve(num_allocs);
  {
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^
                        static_cast<uint64_t>(rd()));
    std::uniform_int_distribution<size_t> dist(min_size, max_size);
    for (size_t i = 0; i < num_allocs; ++i) {
      sizes.push_back(dist(rng));
    }
  }

  // zmalloc
  auto start = Clock::now();
  for (size_t r = 0; r < rounds; ++r) {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(zmalloc::zmalloc(sizes[i]));
    }
    for (void *ptr : ptrs) {
      zmalloc::zfree(ptr);
    }
  }
  auto end = Clock::now();
  auto zmalloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  // malloc
  start = Clock::now();
  for (size_t r = 0; r < rounds; ++r) {
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(malloc(sizes[i]));
    }
    for (void *ptr : ptrs) {
      free(ptr);
    }
  }
  end = Clock::now();
  auto malloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  std::cout << "  zmalloc: " << zmalloc_time << " ms" << std::endl;
  std::cout << "  malloc:  " << malloc_time << " ms" << std::endl;
  std::cout << "  ratio:   " << static_cast<double>(malloc_time) / zmalloc_time
            << "x" << std::endl;
  std::cout << std::endl;
}

void benchmark_random_multi_thread(size_t min_size, size_t max_size,
                                   size_t num_allocs, int thread_count) {
  std::cout << "Multi-thread random-size benchmark: range=[" << min_size << ","
            << max_size << "]" << ", allocs=" << num_allocs
            << ", threads=" << thread_count << std::endl;

  // Pre-generate per-thread size sequences once; reuse for both allocators.
  std::vector<std::vector<size_t>> sizes_per_thread(
      static_cast<size_t>(thread_count));
  {
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^
                        static_cast<uint64_t>(rd()));
    std::uniform_int_distribution<size_t> dist(min_size, max_size);
    for (int t = 0; t < thread_count; ++t) {
      auto &sizes = sizes_per_thread[static_cast<size_t>(t)];
      sizes.reserve(num_allocs);
      for (size_t i = 0; i < num_allocs; ++i) {
        sizes.push_back(dist(rng));
      }
    }
  }

  auto zmalloc_worker = [&](int tid) {
    const auto &sizes = sizes_per_thread[static_cast<size_t>(tid)];
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(zmalloc::zmalloc(sizes[i]));
    }
    for (void *ptr : ptrs) {
      zmalloc::zfree(ptr);
    }
  };

  auto malloc_worker = [&](int tid) {
    const auto &sizes = sizes_per_thread[static_cast<size_t>(tid)];
    std::vector<void *> ptrs;
    ptrs.reserve(num_allocs);
    for (size_t i = 0; i < num_allocs; ++i) {
      ptrs.push_back(malloc(sizes[i]));
    }
    for (void *ptr : ptrs) {
      free(ptr);
    }
  };

  // zmalloc
  auto start = Clock::now();
  {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
      threads.emplace_back(zmalloc_worker, i);
    }
    for (auto &t : threads) {
      t.join();
    }
  }
  auto end = Clock::now();
  auto zmalloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  // malloc
  start = Clock::now();
  {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
      threads.emplace_back(malloc_worker, i);
    }
    for (auto &t : threads) {
      t.join();
    }
  }
  end = Clock::now();
  auto malloc_time =
      std::chrono::duration_cast<Milliseconds>(end - start).count();

  std::cout << "  zmalloc: " << zmalloc_time << " ms" << std::endl;
  std::cout << "  malloc:  " << malloc_time << " ms" << std::endl;
  std::cout << "  ratio:   " << static_cast<double>(malloc_time) / zmalloc_time
            << "x" << std::endl;
  std::cout << std::endl;
}

} // namespace

int main() {
  std::cout << "==============================" << std::endl;
  std::cout << "zmalloc Performance Benchmark" << std::endl;
  std::cout << "==============================" << std::endl << std::endl;

  // 单线程基准
  benchmark_single_thread(8, 100000, 5);
  benchmark_single_thread(64, 100000, 5);
  benchmark_single_thread(1024, 100000, 5);
  benchmark_single_thread(8192, 10000, 5);

  // 多线程基准
  benchmark_multi_thread(64, 100000, 4);
  benchmark_multi_thread(64, 100000, 8);
  benchmark_multi_thread(1024, 50000, 4);

  // 随机大小（1B-8KB）基准
  benchmark_random_single_thread(1, 8 * 1024, 100000, 5);
  benchmark_random_multi_thread(1, 8 * 1024, 100000, 4);

  return 0;
}
