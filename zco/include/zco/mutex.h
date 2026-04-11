#ifndef ZCO_MUTEX_H_
#define ZCO_MUTEX_H_

#include <memory>

#include "zco/internal/noncopyable.h"

namespace zco {

/**
 * @brief 协程友好的互斥锁。
 * @details 在线程上下文使用条件变量阻塞，在协程上下文使用 runtime park/resume。
 */
class Mutex : private NonCopyable {
  public:
    /**
     * @brief 构造互斥锁。
     */
    Mutex();

    /**
     * @brief 析构互斥锁。
     */
    ~Mutex();

    /**
     * @brief 加锁。
     * @return 无返回值。
     */
    void lock() const;

    /**
     * @brief 解锁。
     * @return 无返回值。
     */
    void unlock() const;

    /**
     * @brief 尝试加锁。
     * @return true 表示成功获得锁。
     */
    bool try_lock() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Mutex 的 RAII 守卫。
 */
class MutexGuard : public NonCopyable {
  public:
    /**
     * @brief 构造守卫并加锁。
     * @param mutex 锁对象。
     */
    explicit MutexGuard(const Mutex &mutex);

    /**
     * @brief 构造守卫并加锁。
     * @param mutex 锁对象指针。
     */
    explicit MutexGuard(const Mutex *mutex);

    /**
     * @brief 析构时自动解锁。
     */
    ~MutexGuard();

  private:
    const Mutex *mutex_;
};

} // namespace zco

#endif // ZCO_MUTEX_H_
