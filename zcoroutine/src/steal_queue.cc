#include "zcoroutine/internal/steal_queue.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace zcoroutine {

StealQueue::StealQueue() : mutex_(), size_(0), tasks_() {}

void StealQueue::push(Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.push_back(std::move(task));
  size_.store(tasks_.size(), std::memory_order_relaxed);
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
  size_.store(tasks_.size(), std::memory_order_relaxed);
  return count;
}

void StealQueue::drain_all(std::deque<Task>* tasks) {
  if (!tasks) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  tasks->swap(tasks_);
  size_.store(tasks_.size(), std::memory_order_relaxed);
}

void StealQueue::drain_some(std::deque<Task>* tasks, size_t max_count) {
  if (!tasks || max_count == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const size_t count = std::min(max_count, tasks_.size());
  for (size_t i = 0; i < count; ++i) {
    tasks->push_back(std::move(tasks_.front()));
    tasks_.pop_front();
  }
  size_.store(tasks_.size(), std::memory_order_relaxed);
}

void StealQueue::append(std::deque<Task>* tasks) {
  if (!tasks || tasks->empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  tasks_.insert(tasks_.end(), std::make_move_iterator(tasks->begin()),
                std::make_move_iterator(tasks->end()));
  tasks->clear();
  size_.store(tasks_.size(), std::memory_order_relaxed);
}

size_t StealQueue::size() const { return size_.load(std::memory_order_relaxed); }

}  // namespace zcoroutine
