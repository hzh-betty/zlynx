#include "work_stealing_queue.h"

#include <chrono>
#include <utility>

#include "zcoroutine_logger.h"

namespace zcoroutine {

void WorkStealingQueue::bind_bitmap(StealableQueueBitmap *bitmap, int worker_id,
                                   size_t high_watermark,
                                   size_t low_watermark) {
  bitmap_ = bitmap;
  bitmap_worker_id_ = worker_id;
  high_watermark_ = high_watermark;
  low_watermark_ = low_watermark;

  if (!bitmap_ || bitmap_worker_id_ < 0) {
    ZCOROUTINE_LOG_DEBUG("WorkStealingQueue::bind_bitmap disabled (bitmap={}, worker_id={})",
                        static_cast<const void *>(bitmap_), bitmap_worker_id_);
    return;
  }
  if (high_watermark_ == 0 || low_watermark_ == 0 ||
      low_watermark_ >= high_watermark_) {
    ZCOROUTINE_LOG_WARN(
        "WorkStealingQueue::bind_bitmap invalid watermarks: worker_id={}, H={}, L={} (bitmap disabled)",
        bitmap_worker_id_, high_watermark_, low_watermark_);
    return;
  }

  const size_t cur = approx_size();
  const bool publish = (bitmap_ && bitmap_worker_id_ >= 0 &&
                        cur > high_watermark_ && high_watermark_ > 0);
  bitmap_published_.store(publish, std::memory_order_relaxed);
  if (publish) {
    bitmap_->set(static_cast<size_t>(bitmap_worker_id_));
    ZCOROUTINE_LOG_DEBUG(
        "WorkStealingQueue bitmap publish: worker_id={}, size={}, H={}, L={}",
        bitmap_worker_id_, cur, high_watermark_, low_watermark_);
  }
}

void WorkStealingQueue::maybe_update_bitmap(size_t new_size) {
  if (!bitmap_ || bitmap_worker_id_ < 0) {
    return;
  }
  // 未配置阈值则不启用位图上报。
  if (high_watermark_ == 0 || low_watermark_ == 0 ||
      low_watermark_ >= high_watermark_) {
    return;
  }

  const bool published = bitmap_published_.load(std::memory_order_relaxed);
  if (!published && new_size > high_watermark_) {
    bitmap_->set(static_cast<size_t>(bitmap_worker_id_));
    bitmap_published_.store(true, std::memory_order_relaxed);
    ZCOROUTINE_LOG_DEBUG(
        "WorkStealingQueue bitmap publish: worker_id={}, size={}, H={}, L={}",
        bitmap_worker_id_, new_size, high_watermark_, low_watermark_);
  } else if (published && new_size < low_watermark_) {
    bitmap_->clear(static_cast<size_t>(bitmap_worker_id_));
    bitmap_published_.store(false, std::memory_order_relaxed);
    ZCOROUTINE_LOG_DEBUG(
        "WorkStealingQueue bitmap unpublish: worker_id={}, size={}, H={}, L={}",
        bitmap_worker_id_, new_size, high_watermark_, low_watermark_);
  }
}

void WorkStealingQueue::push(Task &&task) {
  {
    std::lock_guard<Spinlock> lock(lock_);
    tasks_.push_back(std::move(task));
  }
  const size_t old = size_.fetch_add(1, std::memory_order_relaxed);
  maybe_update_bitmap(old + 1);

  // 如果有等待者则唤醒，减少无效的notify调用
  if (waiters_.load(std::memory_order_acquire) > 0) {
    cv_.notify_one();
  }
}

size_t WorkStealingQueue::pop_batch(Task *out, size_t max_count) {
  if (max_count == 0) {
    return 0;
  }

  std::lock_guard<Spinlock> lock(lock_);
  size_t n = 0;
  while (n < max_count && !tasks_.empty()) {
    out[n++] = std::move(tasks_.back());
    tasks_.pop_back();
  }
  if (n > 0) {
    const size_t old = size_.fetch_sub(n, std::memory_order_relaxed);
    maybe_update_bitmap(old - n);
  }
  return n;
}

size_t WorkStealingQueue::steal_batch(Task *out, size_t max_count) {
  if (max_count == 0) {
    return 0;
  }

  std::lock_guard<Spinlock> lock(lock_);
  size_t n = 0;
  while (n < max_count && !tasks_.empty()) {
    out[n++] = std::move(tasks_.front());
    tasks_.pop_front();
  }
  if (n > 0) {
    const size_t old = size_.fetch_sub(n, std::memory_order_relaxed);
    maybe_update_bitmap(old - n);
  }
  return n;
}

size_t WorkStealingQueue::swap_if_empty(WorkStealingQueue &victim) {
  if (&victim == this) {
    return 0;
  }

  // Spinlock 支持 try_lock 后可使用 std::lock 实现无死锁双锁。
  std::unique_lock<Spinlock> l1(lock_, std::defer_lock);
  std::unique_lock<Spinlock> l2(victim.lock_, std::defer_lock);
  std::lock(l1, l2);

  if (!tasks_.empty() || victim.tasks_.empty()) {
    return 0;
  }

  tasks_.swap(victim.tasks_);

  const size_t new_self = tasks_.size();
  const size_t new_victim = victim.tasks_.size();
  size_.store(new_self, std::memory_order_relaxed);
  victim.size_.store(new_victim, std::memory_order_relaxed);

  maybe_update_bitmap(new_self);
  victim.maybe_update_bitmap(new_victim);
  return new_self;
}

size_t WorkStealingQueue::wait_pop_batch(Task *out, size_t max_count,
                                        int timeout_ms) {
  if (max_count == 0) {
    return 0;
  }

  // 快速路径：队列为空时进入条件变量等待（不持有自旋锁）
  if (approx_size() == 0) {
    std::unique_lock<std::mutex> wlock(wait_mutex_);
    waiters_.fetch_add(1, std::memory_order_release);

    const auto pred = [this] {
      return stopped_.load(std::memory_order_relaxed) ||
             size_.load(std::memory_order_relaxed) > 0;
    };

    if (timeout_ms > 0) {
      cv_.wait_for(wlock, std::chrono::milliseconds(timeout_ms), pred);
    } else {
      cv_.wait(wlock, pred);
    }

    waiters_.fetch_sub(1, std::memory_order_release);

    // 超时/停止且仍无任务
    if (stopped_.load(std::memory_order_relaxed) && approx_size() == 0) {
      return 0;
    }
  }

  return pop_batch(out, max_count);
}

void WorkStealingQueue::stop() {
  stopped_.store(true, std::memory_order_relaxed);
  cv_.notify_all();
}

void WorkStealingQueue::notify_one() {
  if (waiters_.load(std::memory_order_acquire) > 0) {
    cv_.notify_one();
  }
}

} // namespace zcoroutine
