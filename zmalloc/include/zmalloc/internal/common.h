/**
 * @file common.h
 * @brief common 定义。
 * @author hzh-betty
 */

#ifndef ZMALLOC_INTERNAL_COMMON_H_
#define ZMALLOC_INTERNAL_COMMON_H_

#include <atomic>
#include <cstddef>
#include <thread>

#include "zmalloc_config.h"

namespace zmalloc {

namespace internal {

#if defined(__GNUC__) || defined(__clang__)
// 常量初始化的递归深度无需 C++ TLS 初始化包装器；保持原有 guard 语义。
extern __thread size_t tls_allocator_call_depth
    __attribute__((tls_model("initial-exec")));
#else
extern thread_local size_t tls_allocator_call_depth;
#endif

class AllocatorCallGuard {
  public:
    AllocatorCallGuard() noexcept { ++tls_allocator_call_depth; }
    ~AllocatorCallGuard() { --tls_allocator_call_depth; }
};

} // namespace internal

class NonCopyable {
  public:
    /** @brief 构造不可复制基类。 */
    NonCopyable() = default;
    /** @brief 默认析构函数。 */
    ~NonCopyable() = default;

    /** @brief 禁止复制，避免派生的锁和缓存对象被意外复制。 */
    NonCopyable(const NonCopyable &) = delete;
    /** @brief 禁止复制赋值。 */
    NonCopyable &operator=(const NonCopyable &) = delete;
};

/**
 * @brief 高性能自旋锁
 *
 * - 使用 load(relaxed) 只读自旋，减少 cache line 抖动
 * - 使用 exchange(acquire) 抢锁，建立同步
 * - unlock 使用 release，形成 happens-before
 * - 指数退避 + 适时 yield，兼顾低竞争与高竞争场景
 */
class alignas(CACHE_LINE_SIZE) SpinLock : public NonCopyable {
  public:
    /** @brief 创建未加锁状态的自旋锁。 */
    SpinLock() noexcept = default;
    /** @brief 获取锁；锁被占用时自旋等待。 */
    void lock() noexcept {
        // 快速路径：立即尝试获取锁
        if (!locked_.exchange(true, std::memory_order_acquire)) {
            return;
        }
        // 慢路径：指数退避自旋
        lock_slow();
    }


    /** @brief 释放锁，并向后续持锁线程发布临界区写入。 */
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

  private:
    static constexpr int kMaxSpinCount = 64;
    std::atomic<bool> locked_{false};

    /** @brief 执行平台相关的短暂让步指令，降低忙等开销。 */
    static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }

    /** @brief 锁竞争时逐步增加自旋间隔，必要时让出线程时间片。 */
    void lock_slow() noexcept {
        int spin_count = 1;
        for (;;) {
            for (int i = 0; i < spin_count; ++i) {
                if (!locked_.load(std::memory_order_relaxed)) {
                    if (!locked_.exchange(true, std::memory_order_acquire)) {
                        return;
                    }
                }
                cpu_relax();
            }

            if (spin_count < kMaxSpinCount) {
                spin_count <<= 1;
            } else {
                std::this_thread::yield();
            }
        }
    }
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_COMMON_H_
