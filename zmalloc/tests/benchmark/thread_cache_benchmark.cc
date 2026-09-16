// 对比单线程、跨线程释放和短命线程；每次运行一个场景，避免 RSS 相互污染。
#include "zmalloc/zmalloc.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/resource.h>

namespace {
constexpr size_t kBatch = 256;
constexpr size_t kRounds = 40000;

void allocate_batch(void **objects, size_t size) {
    for (size_t i = 0; i < kBatch; ++i) {
        objects[i] = zmalloc::zmalloc(size);
        static_cast<char *>(objects[i])[0] = 42;
    }
}

void free_batch(void **objects) {
    for (size_t i = 0; i < kBatch; ++i) {
        zmalloc::zfree(objects[i]);
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || (std::strcmp(argv[1], "single") != 0 &&
                      std::strcmp(argv[1], "cross") != 0 &&
                      std::strcmp(argv[1], "churn") != 0)) {
        std::fprintf(stderr, "Usage: %s single|cross|churn BYTES\n", argv[0]);
        return 1;
    }
    char *end = nullptr;
    const size_t size = std::strtoul(argv[2], &end, 10);
    if (*end != '\0' || size == 0 || size > zmalloc::MAX_BYTES) {
        return 1;
    }
    const size_t rounds = std::strcmp(argv[1], "churn") == 0 ? 4000 : kRounds;
    const auto start = std::chrono::steady_clock::now();
    if (std::strcmp(argv[1], "single") == 0) {
        void *objects[kBatch];
        for (size_t r = 0; r < rounds; ++r) {
            allocate_batch(objects, size);
            free_batch(objects);
        }
    } else if (std::strcmp(argv[1], "cross") == 0) {
        std::array<std::atomic<void *>, kBatch> slots{};
        for (auto &slot : slots) {
            slot.store(nullptr, std::memory_order_relaxed);
        }
        std::thread producer([&] {
            for (size_t r = 0; r < rounds; ++r) {
                for (auto &slot : slots) {
                    while (slot.load(std::memory_order_acquire) != nullptr) {
                        std::this_thread::yield();
                    }
                    void *p = zmalloc::zmalloc(size);
                    static_cast<char *>(p)[0] = 42;
                    slot.store(p, std::memory_order_release);
                }
            }
        });
        std::thread consumer([&] {
            for (size_t r = 0; r < rounds; ++r) {
                for (auto &slot : slots) {
                    void *p;
                    while ((p = slot.load(std::memory_order_acquire)) == nullptr) {
                        std::this_thread::yield();
                    }
                    zmalloc::zfree(p);
                    slot.store(nullptr, std::memory_order_release);
                }
            }
        });
        producer.join();
        consumer.join();
    } else {
        for (size_t r = 0; r < rounds; ++r) {
            std::thread worker([&] {
                void *objects[kBatch];
                allocate_batch(objects, size);
                free_batch(objects);
            });
            worker.join();
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    // /proc 的峰值仅针对当前地址空间，避免继承启动器的 ru_maxrss。
    long peak_rss = usage.ru_maxrss;
    long rss = 0;
    if (FILE *status = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), status)) {
            std::sscanf(line, "VmHWM: %ld kB", &peak_rss);
            std::sscanf(line, "VmRSS: %ld kB", &rss);
        }
        std::fclose(status);
    }
    std::printf("scenario=%s size=%zu pairs=%zu seconds=%.6f Mpairs/s=%.3f "
                "peak_rss_kib=%ld rss_kib=%ld\n", argv[1], size, kBatch * rounds,
                seconds, kBatch * rounds / seconds / 1e6, peak_rss, rss);
}
