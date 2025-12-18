#ifndef ZCOROUTINE_SPINLOCK_H_
#define ZCOROUTINE_SPINLOCK_H_

#include <atomic>
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 自旋锁类
 * 使用std::atomic_flag实现（C++11标准，无锁原子操作）
 * 适用于临界区代码执行时间极短的场景
 */
class Spinlock : public NonCopyable {
public:
    Spinlock() : flag_(ATOMIC_FLAG_INIT) {}

    /**
     * @brief 加锁
     * 循环等待直到获取锁
     */
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // 自旋等待
        }
    }

    /**
     * @brief 解锁
     */
    void unlock() {
        flag_.clear(std::memory_order_release);
    }

    /**
     * @brief 尝试加锁
     * @return true表示成功获取锁，false表示锁已被占用
     */
    bool try_lock() {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

private:
    std::atomic_flag flag_;     // 原子标志
};

/**
 * @brief 自旋锁RAII封装
 */
class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& spinlock)
        : spinlock_(spinlock) {
        spinlock_.lock();
    }

    ~SpinlockGuard() {
        spinlock_.unlock();
    }

private:
    Spinlock& spinlock_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SPINLOCK_H_
