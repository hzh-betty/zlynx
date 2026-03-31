#include "zcoroutine/internal/steal_queue.h"

#include <algorithm>
#include <utility>

namespace zcoroutine {

StealQueue::StealQueue() : mutex_(), tasks_() {}

void StealQueue::push(Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.push_back(std::move(task));
}

size_t StealQueue::steal(std::deque<Task>* tasks, size_t max_steal, size_t min_reserve) {
  if (!tasks || max_steal == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const size_t total = tasks_.size();
  if (total <= min_reserve) {
    return 0;
  }

  size_t target = 1;
  if (total >= 64) {
    target = (total * 2) / 5;
  } else if (total >= 8) {
    target = total / 3;
  }

  if (target == 0) {
    target = 1;
  }

  const size_t stealable = total - min_reserve;
  const size_t count = std::min(max_steal, std::min(target, stealable));
  for (size_t i = 0; i < count; ++i) {
    tasks->push_back(std::move(tasks_.back()));
    tasks_.pop_back();
  }
  return count;
}

void StealQueue::drain_all(std::deque<Task>* tasks) {
  if (!tasks) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  tasks->swap(tasks_);
}

void StealQueue::append(std::deque<Task>* tasks) {
  if (!tasks || tasks->empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  while (!tasks->empty()) {
    tasks_.push_back(std::move(tasks->front()));
    tasks->pop_front();
  }
}

size_t StealQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

}  // namespace zcoroutine