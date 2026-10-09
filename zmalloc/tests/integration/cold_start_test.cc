#include "zmalloc/zmalloc.h"

#include <atomic>
#include <cstring>
#include <thread>

int main() {
    // 不链接 override，也不提前调用 zmalloc，保证两个单例在冷启动时并行构造。
    std::atomic<int> ready(0);
    std::atomic<bool> go(false);
    std::atomic<bool> valid(true);
    auto allocate = [&](size_t size) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto *ptr = static_cast<unsigned char *>(zmalloc::zmalloc(size));
        if (ptr == nullptr) {
            valid.store(false, std::memory_order_relaxed);
            return;
        }
        std::memset(ptr, 0x5a, size);
        if (ptr[0] != 0x5a || ptr[size - 1] != 0x5a) {
            valid.store(false, std::memory_order_relaxed);
        }
        zmalloc::zfree(ptr);
    };
    std::thread small(allocate, 64);
    std::thread large(allocate, 512 * 1024);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    small.join();
    large.join();
    return valid.load(std::memory_order_relaxed) ? 0 : 1;
}
