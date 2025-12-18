#include "scheduling/task_queue.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

void TaskQueue::push(const Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(task);
    cv_.notify_one();  // 唤醒一个等待的线程
}

bool TaskQueue::pop(Task& task) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待直到有任务或队列停止
    cv_.wait(lock, [this]() {
        return !tasks_.empty() || stopped_;
    });
    
    if (stopped_ && tasks_.empty()) {
        return false;
    }
    
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

bool TaskQueue::try_pop(Task& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (tasks_.empty()) {
        return false;
    }
    
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

bool TaskQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.empty();
}

void TaskQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();  // 唤醒所有等待的线程
}

} // namespace zcoroutine
