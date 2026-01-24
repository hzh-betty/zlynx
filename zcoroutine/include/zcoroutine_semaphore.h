#ifndef ZCOROUTINE_SEMAPHORE_H_
#define ZCOROUTINE_SEMAPHORE_H_

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

/**
 * @brief Linux 原生信号量封装（基于 eventfd + EFD_SEMAPHORE）
 *
 * 语义：
 * - post()：计数 +1
 * - wait()：计数 -1（若为 0 则阻塞）
 *
 * @note
 * - 使用 eventfd(EFD_SEMAPHORE)，每次 read() 返回 1 并递减计数。
 * - wait() 会处理 EINTR 并重试。
 */
class Semaphore : public NonCopyable {
public:
  explicit Semaphore(uint64_t initial = 0)
      : fd_(::eventfd(initial, EFD_SEMAPHORE | EFD_CLOEXEC)) {}

  ~Semaphore() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void post() {
    const uint64_t one = 1;
    while (::write(fd_, &one, sizeof(one)) == -1 && errno == EINTR) {
    }
  }

  void wait() {
    uint64_t value = 0;
    while (::read(fd_, &value, sizeof(value)) == -1 && errno == EINTR) {
    }
  }

private:
  int fd_{-1};
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SEMAPHORE_H_
