/**
 * @file spinlock.cc
 * @brief 自旋锁的等待实现。
 */

#include "spinlock.h"

namespace zlog {
namespace detail {

void Spinlock::lock_slow() noexcept {
    {
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
}

} // namespace detail
} // namespace zlog
