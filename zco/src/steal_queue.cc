#include "zco/internal/steal_queue.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace zco {

// StealQueue 是任务投递的工作窃取缓冲：
// - push() 在队尾追加，保持生产者本地写入简单。
// - steal() 从队尾批量拿走任务，尽量保留队首较早进入的任务，减少局部性破坏。
// - drain_some() 只在调度线程自消费路径使用，用于把外部任务批量转为 Fiber。

StealQueue::StealQueue() : mutex_(), size_(0), tasks_() {}

void StealQueue::push(Task task) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
    size_.store(tasks_.size(), std::memory_order_relaxed);
}

size_t StealQueue::steal(std::deque<Task> *tasks, size_t max_steal,
                         size_t min_reserve) {
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
        // 队列较长时提高窃取比例，让空闲调度器更快分担热点负载。
        target = (total * 2) / 5;
    } else if (total >= 8) {
        // 中等队列选择更保守的分批比例，避免一次性搬空造成抖动。
        target = total / 3;
    }

    if (target == 0) {
        target = 1;
    }

    const size_t stealable = total - min_reserve;
    const size_t count = std::min(max_steal, std::min(target, stealable));
    for (size_t i = 0; i < count; ++i) {
        // 从队尾窃取，优先拿走最新任务，降低原队列前端等待时间。
        tasks->push_back(std::move(tasks_.back()));
        tasks_.pop_back();
    }
    size_.store(tasks_.size(), std::memory_order_relaxed);
    return count;
}

void StealQueue::drain_all(std::deque<Task> *tasks) {
    if (!tasks) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    tasks->swap(tasks_);
    size_.store(tasks_.size(), std::memory_order_relaxed);
}

void StealQueue::drain_some(std::deque<Task> *tasks, size_t max_count) {
    if (!tasks || max_count == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const size_t count = std::min(max_count, tasks_.size());
    for (size_t i = 0; i < count; ++i) {
        // 自消费路径按 FIFO 取出，保证提交顺序与调度顺序尽量一致。
        tasks->push_back(std::move(tasks_.front()));
        tasks_.pop_front();
    }
    size_.store(tasks_.size(), std::memory_order_relaxed);
}

void StealQueue::append(std::deque<Task> *tasks) {
    if (!tasks || tasks->empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.insert(tasks_.end(), std::make_move_iterator(tasks->begin()),
                  std::make_move_iterator(tasks->end()));
    tasks->clear();
    size_.store(tasks_.size(), std::memory_order_relaxed);
}

size_t StealQueue::size() const {
    return size_.load(std::memory_order_relaxed);
}

} // namespace zco
