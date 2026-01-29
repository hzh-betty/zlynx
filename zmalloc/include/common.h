#ifndef ZMALLOC_COMMON_H_
#define ZMALLOC_COMMON_H_

#include <atomic>
#include <thread>

namespace zmalloc {

class NonCopyable {
public:
  NonCopyable() = default;
  ~NonCopyable() = default;

  // 禁止拷贝构造
  NonCopyable(const NonCopyable &) = delete;
  // 禁止拷贝赋值
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
class alignas(64) SpinLock: public NonCopyable {
public:
  SpinLock() noexcept = default;
  void lock() noexcept {
    // 快速路径：立即尝试获取锁
    if (!locked_.exchange(true, std::memory_order_acquire)) {
      return;
    }
    // 慢路径：指数退避自旋
    lock_slow();
  }

  /**
   * @brief 尝试获取锁（非阻塞）
   * @return true: 成功获取锁; false: 锁被占用
   */
  bool try_lock() noexcept {
    return !locked_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
  static constexpr int kMaxSpinCount = 64;
  std::atomic<bool> locked_{false};

  static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
  }

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

#endif // ZMALLOC_COMMON_H_
