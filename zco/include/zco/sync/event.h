#pragma once
#include "zco/detail/wait_queue.h"
#include <mutex>

namespace zco {
class Event {
  public:
    explicit Event(bool manual_reset = false, bool signaled = false)
        : manual_reset_(manual_reset), signaled_(signaled) {}

    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
    Result<void> wait(Deadline deadline = {});
    void signal();
    void reset();

  private:
    std::mutex mutex_;
    detail::WaitQueue waiters_;
    bool manual_reset_, signaled_;
};
} // namespace zco
