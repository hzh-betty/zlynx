#ifndef ZCOROUTINE_INTERNAL_STEAL_QUEUE_H_
#define ZCOROUTINE_INTERNAL_STEAL_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "zcoroutine/internal/noncopyable.h"
#include "zcoroutine/sched.h"

namespace zcoroutine {

class StealQueue : public NonCopyable {
 public:
  StealQueue();

  void push(Task task);

  size_t steal(std::deque<Task>* tasks, size_t max_steal, size_t min_reserve);

  void drain_all(std::deque<Task>* tasks);

  void append(std::deque<Task>* tasks);

  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::deque<Task> tasks_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_STEAL_QUEUE_H_