#pragma once
#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
class WaitGroup {
  public:
    explicit WaitGroup(size_t count = 0) : count_(count) {}

    WaitGroup(const WaitGroup &) = delete;
    WaitGroup &operator=(const WaitGroup &) = delete;
    void add(size_t count = 1);
    void done();
    Result<void> wait(Deadline deadline = {});

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    size_t count_;
};
} // namespace zco
