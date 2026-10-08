#pragma once
#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
class Mutex {
  public:
    Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    Result<void> lock(Deadline deadline = {});
    bool try_lock();
    void unlock();

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    bool locked_ = false;
};
} // namespace zco
