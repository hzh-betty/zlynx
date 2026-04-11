#ifndef ZCO_INTERNAL_STEAL_QUEUE_H_
#define ZCO_INTERNAL_STEAL_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "zco/internal/noncopyable.h"
#include "zco/sched.h"

namespace zco {

class StealQueue : public NonCopyable {
  public:
    StealQueue();

    void push(Task task);

    size_t steal(std::deque<Task> *tasks, size_t max_steal, size_t min_reserve);

    void drain_all(std::deque<Task> *tasks);

    void drain_some(std::deque<Task> *tasks, size_t max_count);

    void append(std::deque<Task> *tasks);

    size_t size() const;

  private:
    mutable std::mutex mutex_;
    std::atomic<size_t> size_;
    std::deque<Task> tasks_;
};

} // namespace zco

#endif // ZCO_INTERNAL_STEAL_QUEUE_H_
