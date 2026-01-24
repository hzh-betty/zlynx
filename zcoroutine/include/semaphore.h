#ifndef ZCOROUTINE_SEMAPHORE_H_
#define ZCOROUTINE_SEMAPHORE_H_

#include <semaphore.h>

#include <cerrno>

#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

/**
 * @brief Linux(POSIX) 信号量封装（线程内同步用，进程共享未启用）
 *
 * 基于 sem_t：
 * - post()：计数 +1
 * - wait()：计数 -1（若为 0 则阻塞）
 *
 * @note
 * - 使用 sem_init(pshared=0)，仅线程间同步。
 * - wait() 会处理 EINTR 重试。
 */
class Semaphore : public NonCopyable {
public:
  explicit Semaphore(unsigned int initial = 0) {
    sem_init(&sem_, 0, initial);
  }

  ~Semaphore() { sem_destroy(&sem_); }

  void post() { sem_post(&sem_); }

  void wait() {
    while (sem_wait(&sem_) == -1 && errno == EINTR) {
    }
  }

private:
  sem_t sem_{};
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SEMAPHORE_H_
