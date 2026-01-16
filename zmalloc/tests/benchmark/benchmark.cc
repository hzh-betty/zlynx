/**
 * @file benchmark.cc
 * @brief zmalloc 性能基准测试
 */

#include "zmalloc.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
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
      ptrs.push_back(std::malloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      std::free(ptr);
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
      ptrs.push_back(std::malloc(alloc_size));
    }
    for (void *ptr : ptrs) {
      std::free(ptr);
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

  return 0;
}
