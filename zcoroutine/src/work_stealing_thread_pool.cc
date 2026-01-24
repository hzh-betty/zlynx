#include "work_stealing_thread_pool.h"

#include "work_stealing_queue.h"

namespace zcoroutine {

WorkStealingThreadPool::WorkStealingThreadPool(int thread_count,
                                               std::string name)
    : name_(std::move(name)), thread_count_(thread_count),
      work_queues_(static_cast<size_t>(thread_count_)),
      stealable_bitmap_(static_cast<size_t>(thread_count_)) {
  processors_.reserve(static_cast<size_t>(thread_count_));
  for (int i = 0; i < thread_count_; ++i) {
    processors_.push_back(std::make_unique<Processor>(i));
  }

  for (int i = 0; i < thread_count_; ++i) {
    work_queues_[static_cast<size_t>(i)].store(&processors_[static_cast<size_t>(i)]->run_queue,
                                               std::memory_order_relaxed);
  }
}

WorkStealingThreadPool::~WorkStealingThreadPool() { stop(); }

void WorkStealingThreadPool::start(const std::function<void(int)> &worker_entry) {
  if (!threads_.empty()) {
    return;
  }

  // Processor 及其队列在构造时已创建并发布，start() 只负责拉起线程。
  rr_enqueue_.store(0, std::memory_order_relaxed);
  registered_worker_queues_.store(thread_count_, std::memory_order_relaxed);

  threads_.reserve(static_cast<size_t>(thread_count_));
  for (int i = 0; i < thread_count_; ++i) {
    auto thread = std::make_unique<std::thread>([worker_entry, i]() {
      worker_entry(i);
    });
    threads_.push_back(std::move(thread));
  }

  wait_for_all_queues_registered();
}

void WorkStealingThreadPool::stop() {
  stop_work_queues();

  for (auto &thread : threads_) {
    if (thread && thread->joinable()) {
      thread->join();
    }
  }
  threads_.clear();
}

void WorkStealingThreadPool::register_work_queue(int worker_id,
                                                 WorkStealingQueue *queue) {
  if (!queue) {
    return;
  }
  if (worker_id < 0 || worker_id >= thread_count_) {
    return;
  }

  // 兼容接口：允许上层显式注册/覆盖队列指针。
  // 当前默认由 Processor 创建并发布，因此通常无需调用。
  work_queues_[static_cast<size_t>(worker_id)].store(queue,
                                                     std::memory_order_release);
}

WorkStealingQueue *WorkStealingThreadPool::get_next_queue(int worker_id) const {
  if (worker_id < 0 || worker_id >= thread_count_) {
    return nullptr;
  }
  return work_queues_[static_cast<size_t>(worker_id)].load(
      std::memory_order_acquire);
}

void WorkStealingThreadPool::stop_work_queues() {
  for (int i = 0; i < thread_count_; ++i) {
    if (auto *q = get_next_queue(i)) {
      q->stop();
    }
  }
}

WorkStealingQueue *WorkStealingThreadPool::local_queue(int worker_id) const {
  return get_next_queue(worker_id);
}

void WorkStealingThreadPool::wait_for_all_queues_registered() {
  if (thread_count_ <= 0) {
    return;
  }

  std::unique_lock<std::mutex> lock(start_mutex_);
  start_cv_.wait(lock, [this] {
    return registered_worker_queues_.load(std::memory_order_acquire) >=
           thread_count_;
  });
}

} // namespace zcoroutine
