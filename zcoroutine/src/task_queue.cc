#include "task_queue.h"
#include <chrono>
#include <mutex>

namespace zcoroutine {

void TaskQueue::push(const Task &task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(task);
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  // 仅在有等待者时才唤醒，减少无效的notify调用
  if (waiters_.load(std::memory_order_acquire) > 0) {
    cv_.notify_one();
  }
}

void TaskQueue::push(Task &&task) {
  {
    std::lock_guard<Spinlock> lock(spinlock_);
    tasks_.push(std::move(task));
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  // 仅在有等待者时才唤醒，减少无效的notify调用
  if (waiters_.load(std::memory_order_acquire) > 0) {
    cv_.notify_one();
  }
}

bool TaskQueue::try_pop(Task &task) {
  // 快速路径：先检查size，避免无谓加锁
  if (size_.load(std::memory_order_relaxed) == 0) {
    return false;
  }

  std::lock_guard<Spinlock> lock(spinlock_);
  if (!tasks_.empty()) {
    task = std::move(tasks_.front());
    tasks_.pop();
    size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool TaskQueue::pop(Task &task, int timeout_ms) {
  // 快速路径：多次自旋尝试，避免立即进入条件变量等待
  static constexpr int kSpinTries = 16;
  for (int i = 0; i < kSpinTries; ++i) {
    if (try_pop(task)) {
      return true;
    }
    // 短暂自旋
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
  }

  // 快速路径失败，进入阻塞等待
  std::unique_lock<Spinlock> lock(spinlock_);

  // 增加等待者计数
  waiters_.fetch_add(1, std::memory_order_release);

  bool result = false;
  if (timeout_ms > 0) {
    // 带超时等待
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    cv_.wait_until(lock, deadline,
                   [this] { return stopped_ || !tasks_.empty(); });
  } else {
    // 永久等待
    cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
  }

  // 减少等待者计数
  waiters_.fetch_sub(1, std::memory_order_release);

  if (!tasks_.empty()) {
    task = std::move(tasks_.front());
    tasks_.pop();
    size_.fetch_sub(1, std::memory_order_relaxed);
    result = true;
  }

  return result;
}

size_t TaskQueue::size() const { return size_.load(std::memory_order_relaxed); }

bool TaskQueue::empty() const {
  return size_.load(std::memory_order_relaxed) == 0;
}

void TaskQueue::stop() {
  {
    // 必须在与 pop() 相同的锁保护下更新 stopped_，否则可能出现：
    // consumer 在检查谓词后准备进入 wait 之前错过 notify，导致永久阻塞。
    std::lock_guard<Spinlock> lock(spinlock_);
    stopped_.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
}

} // namespace zcoroutine
