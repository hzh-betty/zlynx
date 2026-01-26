/**
 * @file performance.cc
 * @brief zmalloc performance driver (for perf analysis)
 *
 * Example:
 *   ./zmalloc_performance --threads 8 --size 64 --allocs 200000 --rounds 20
 *
 * Notes:
 * - 仅测试 zmalloc/zfree 路径，适合 perf/callgrind 分析。
 * - --touch 会写入 1 字节，避免某些场景下分配后不触碰内存带来的偏差。
 */

#include "zmalloc.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

namespace {

struct Options {
  int threads = 4;
  size_t min_alloc_size = 1;
  size_t max_alloc_size = 8 * 1024;
  size_t num_allocs = 100000;
  size_t rounds = 10;
  bool touch_memory = false;
};

static void print_usage(const char *argv0) {
  std::cout
      << "Usage: " << argv0 << " [options]\n\n"
      << "Options:\n"
      << "  -t, --threads N        Thread count (default: 4)\n"
      << "  -s, --size BYTES       (compat) Fixed size; equals "
         "--min-size/--max-size\n"
      << "      --min-size BYTES   Min allocation size (default: 1)\n"
      << "      --max-size BYTES   Max allocation size (default: 8192)\n"
      << "  -n, --allocs N         Allocations per thread per round (default: "
         "100000)\n"
      << "  -r, --rounds N         Rounds (default: 10)\n"
      << "      --touch            Touch allocated memory (write 1 byte)\n"
      << "  -h, --help             Show this help\n\n"
      << "Examples:\n"
      << "  " << argv0
      << " --threads 8 --min-size 1 --max-size 8192 --allocs 200000 --rounds "
         "20\n"
      << "  " << argv0 << " -t 4 -s 1024 -n 50000 -r 30 --touch\n";
}

static bool parse_size_t(const char *s, size_t *out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return false;
  }
  *out = static_cast<size_t>(v);
  return true;
}

static bool parse_int(const char *s, int *out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

static bool parse_args(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];

    auto require_value = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return false;
    }

    if (a == "-t" || a == "--threads") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      int n = 0;
      if (!parse_int(v, &n) || n <= 0) {
        std::cerr << "Invalid threads: " << v << "\n";
        return false;
      }
      opt->threads = n;
      continue;
    }

    if (a == "-s" || a == "--size") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      size_t n = 0;
      if (!parse_size_t(v, &n) || n == 0) {
        std::cerr << "Invalid size: " << v << "\n";
        return false;
      }
      opt->min_alloc_size = n;
      opt->max_alloc_size = n;
      continue;
    }

    if (a == "--min-size") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      size_t n = 0;
      if (!parse_size_t(v, &n) || n == 0) {
        std::cerr << "Invalid min-size: " << v << "\n";
        return false;
      }
      opt->min_alloc_size = n;
      continue;
    }

    if (a == "--max-size") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      size_t n = 0;
      if (!parse_size_t(v, &n) || n == 0) {
        std::cerr << "Invalid max-size: " << v << "\n";
        return false;
      }
      opt->max_alloc_size = n;
      continue;
    }

    if (a == "-n" || a == "--allocs") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      size_t n = 0;
      if (!parse_size_t(v, &n) || n == 0) {
        std::cerr << "Invalid allocs: " << v << "\n";
        return false;
      }
      opt->num_allocs = n;
      continue;
    }

    if (a == "-r" || a == "--rounds") {
      const char *v = require_value(a.c_str());
      if (!v)
        return false;
      size_t n = 0;
      if (!parse_size_t(v, &n) || n == 0) {
        std::cerr << "Invalid rounds: " << v << "\n";
        return false;
      }
      opt->rounds = n;
      continue;
    }

    if (a == "--touch") {
      opt->touch_memory = true;
      continue;
    }

    std::cerr << "Unknown argument: " << a << "\n";
    print_usage(argv[0]);
    return false;
  }

  if (opt->min_alloc_size > opt->max_alloc_size) {
    std::cerr << "Invalid range: min-size(" << opt->min_alloc_size
              << ") > max-size(" << opt->max_alloc_size << ")\n";
    return false;
  }
  return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void
do_not_optimize_ptr(void *p) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(p) : "memory");
#else
  (void)p;
#endif
}

static long long run_zmalloc(const Options &opt) {
  std::atomic<uintptr_t> sink{0};

  auto worker = [&](int /*tid*/) {
    // Thread-local RNG to avoid contention and keep distributions independent.
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^
                        static_cast<uint64_t>(rd()));
    std::uniform_int_distribution<size_t> dist(opt.min_alloc_size,
                                               opt.max_alloc_size);

    uintptr_t local = 0;
    for (size_t r = 0; r < opt.rounds; ++r) {
      std::vector<void *> ptrs;
      ptrs.reserve(opt.num_allocs);

      for (size_t i = 0; i < opt.num_allocs; ++i) {
        const size_t size = dist(rng);
        void *p = zmalloc::zmalloc(size);
        if (opt.touch_memory && p != nullptr) {
          *reinterpret_cast<volatile unsigned char *>(p) = 0xA5;
        }
        ptrs.push_back(p);
        local ^= reinterpret_cast<uintptr_t>(p);
      }

      for (void *p : ptrs) {
        zmalloc::zfree(p);
      }
    }
    sink.fetch_xor(local, std::memory_order_relaxed);
  };

  auto start = Clock::now();
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(opt.threads));
  for (int i = 0; i < opt.threads; ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }
  auto end = Clock::now();

  // prevent whole-program cleverness
  do_not_optimize_ptr(
      reinterpret_cast<void *>(sink.load(std::memory_order_relaxed)));

  return std::chrono::duration_cast<Milliseconds>(end - start).count();
}

} // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) {
    return 1;
  }

  std::cout << "==============================" << std::endl;
  std::cout << "zmalloc perf driver" << std::endl;
  std::cout << "==============================" << std::endl;
  std::cout << "threads=" << opt.threads << ", size=[" << opt.min_alloc_size
            << "," << opt.max_alloc_size << "]" << ", allocs=" << opt.num_allocs
            << ", rounds=" << opt.rounds
            << ", touch=" << (opt.touch_memory ? "true" : "false") << std::endl;

  // small warmup to stabilize caches / thread startup costs
  {
    Options warm = opt;
    warm.rounds = 1;
    warm.num_allocs = std::min<size_t>(opt.num_allocs, 1000);
    (void)run_zmalloc(warm);
  }

  const long long zmalloc_ms = run_zmalloc(opt);
  std::cout << "zmalloc: " << zmalloc_ms << " ms" << std::endl;

  return 0;
}
