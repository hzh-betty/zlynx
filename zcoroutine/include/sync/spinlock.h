#ifndef ZCOROUTINE_SPINLOCK_H_
#define ZCOROUTINE_SPINLOCK_H_
#include <atomic>
#include <thread>

#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 高性能自旋锁（指数退避 + 自适应）
 *
 * 设计要点：
 * 1. load(relaxed) 只读自旋，减少 cache line 抖动
 * 2. exchange(acquire) 真正抢锁，建立同步
 * 3. unlock 使用 release，形成 happens-before
 * 4. 指数退避策略：缩短平均自旋时间，减少yield调用

 */
class alignas(64) Spinlock : public NonCopyable {
public:
  Spinlock() noexcept = default;

  void lock() noexcept {
    // 快速路径：立即尝试获取锁
    if (!locked_.exchange(true, std::memory_order_acquire)) {
      return;
    }

    // 慢速路径：指数退避自旋
    lock_slow();
  }

  bool try_lock() noexcept {
    bool expected = false;
    return locked_.compare_exchange_strong(expected, true,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed);
  }

  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
  static constexpr int kMaxSpinCount = 64; // 最大自旋次数

  std::atomic<bool> locked_{false};

  void lock_slow() noexcept {
    int spin_count = 1;

    for (;;) {
      // 第一阶段：只读自旋（指数退避）
      for (int i = 0; i < spin_count; ++i) {
        if (!locked_.load(std::memory_order_relaxed)) {
          // 尝试抢锁
          if (!locked_.exchange(true, std::memory_order_acquire)) {
            return;
          }
        }
        cpu_relax();
      }

      // 指数退避：翻倍自旋次数，但不超过上限
      if (spin_count < kMaxSpinCount) {
        spin_count <<= 1;
      } else {
        // 达到上限，让出CPU
        std::this_thread::yield();
      }
    }
  }

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

} // namespace zcoroutine

#endif // ZCOROUTINE_SPINLOCK_H_
