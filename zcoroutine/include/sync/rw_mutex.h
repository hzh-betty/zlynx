#ifndef ZCOROUTINE_RW_MUTEX_H_
#define ZCOROUTINE_RW_MUTEX_H_

#include <pthread.h>

#include "util/noncopyable.h"

namespace zcoroutine {

// 前向声明
class RWMutex;

/**
 * @brief 读锁RAII封装
 */
template <class T> class ReadLockGuard {
public:
  explicit ReadLockGuard(T &rwlock) : rwmutex_(rwlock), locked_(false) {
    lock();
  }

  ~ReadLockGuard() { unlock(); }

  void lock() {
    if (!locked_) {
      rwmutex_.rdlock();
      locked_ = true;
    }
  }

  void unlock() {
    if (locked_) {
      rwmutex_.unlock();
      locked_ = false;
    }
  }

private:
  T &rwmutex_;  // 读写锁对象
  bool locked_; // 锁定状态
};

/**
 * @brief 写锁RAII封装
 */
template <class T> class WriteLockGuard {
public:
  explicit WriteLockGuard(T &rwlock) : rwmutex_(rwlock), locked_(false) {
    lock();
  }

  ~WriteLockGuard() { unlock(); }

  void lock() {
    if (!locked_) {
      rwmutex_.wrlock();
      locked_ = true;
    }
  }

  void unlock() {
    if (locked_) {
      rwmutex_.unlock();
      locked_ = false;
    }
  }

private:
  T &rwmutex_;  // 读写锁对象
  bool locked_; // 锁定状态
};

/**
 * @brief 读写锁类
 * 使用pthread_rwlock_t实现
 * C++14标准库没有提供读写锁，因此保留pthread实现
 */
class RWMutex : public NonCopyable {
public:
  using ReadLock = ReadLockGuard<RWMutex>;
  using WriteLock = WriteLockGuard<RWMutex>;

  RWMutex() { pthread_rwlock_init(&rwlock_, nullptr); }

  ~RWMutex() { pthread_rwlock_destroy(&rwlock_); }

  /**
   * @brief 获取读锁
   */
  void rdlock() { pthread_rwlock_rdlock(&rwlock_); }

  /**
   * @brief 获取写锁
   */
  void wrlock() { pthread_rwlock_wrlock(&rwlock_); }

  /**
   * @brief 解锁
   */
  void unlock() { pthread_rwlock_unlock(&rwlock_); }

private:
  pthread_rwlock_t rwlock_{}; // 底层读写锁对象
};

} // namespace zcoroutine

#endif // ZCOROUTINE_RW_MUTEX_H_
