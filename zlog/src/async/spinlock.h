/**
 * @file spinlock.h
 * @brief 异步执行器内部的自旋锁。
 */

#ifndef ZLOG_ASYNC_SPINLOCK_H_
#define ZLOG_ASYNC_SPINLOCK_H_

#include <atomic>
#include <thread>

namespace zlog {
namespace detail {

/**
 * @brief 高性能自旋锁（指数退避 + 自适应）
 *
 * 设计要点：
 * 1. load(relaxed) 只读自旋，减少 cache line 抖动
 * 2. exchange(acquire) 真正抢锁，建立同步
 * 3. unlock 使用 release，形成 happens-before
 * 4. 指数退避策略：缩短平均自旋时间，减少yield调用
 */
class alignas(64) Spinlock {
  public:
    Spinlock() noexcept = default;
    Spinlock(const Spinlock &) = delete;
    Spinlock &operator=(const Spinlock &) = delete;

    void lock() noexcept {
        // 快速路径：立即尝试获取锁
        if (!locked_.exchange(true, std::memory_order_acquire)) {
            return;
        }

        // 慢速路径：指数退避自旋
        lock_slow();
    }

    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

  private:
    static constexpr int kMaxSpinCount = 64; // 最大自旋次数

    std::atomic<bool> locked_{false};

    void lock_slow() noexcept;

    static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }
};

} // namespace detail
} // namespace zlog

#endif // ZLOG_ASYNC_SPINLOCK_H_
