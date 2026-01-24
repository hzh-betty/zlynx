#include "work_stealing_thread_pool.h"

#include "work_stealing_queue.h"

namespace zcoroutine {

WorkStealingThreadPool::WorkStealingThreadPool(int thread_count,
                                               std::string name)
    : name_(std::move(name)), thread_count_(thread_count),
      work_queues_(static_cast<size_t>(thread_count_)),
      stealable_bitmap_(static_cast<size_t>(thread_count_)) {

  // 创建 Processor 
  processors_.reserve(static_cast<size_t>(thread_count_));
  for (int i = 0; i < thread_count_; ++i) {
    processors_.push_back(std::make_unique<Processor>(i));
  }

  // 初始化队列指针为 nullptr
  for (int i = 0; i < thread_count_; ++i) {
    work_queues_[static_cast<size_t>(i)].store(nullptr, std::memory_order_relaxed);
  }
}

WorkStealingThreadPool::~WorkStealingThreadPool() { stop(); }

void WorkStealingThreadPool::start(const std::function<void(int)> &worker_entry) {
  if (!threads_.empty()) {
    return;
  }

  // Processor 及其队列在构造时已创建；start() 负责：拉起线程 + 发布队列指针。
  rr_enqueue_.store(0, std::memory_order_relaxed);

  threads_.reserve(static_cast<size_t>(thread_count_));
  for (int i = 0; i < thread_count_; ++i) {
    auto thread = std::make_unique<std::thread>([this, worker_entry, i]() {
      // 先发布队列，使得其他线程可以通过 get_next_queue() 找到它。
      publish_worker_queue(i);
      worker_entry(i);
    });
    threads_.push_back(std::move(thread));
  }

  // 启动屏障：等待所有队列完成发布。
  for (int i = 0; i < thread_count_; ++i) {
    start_sem_.wait();
  }
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
  if (worker_id < 0 || worker_id >= thread_count_) {
    return nullptr;
  }
  return &processors_[static_cast<size_t>(worker_id)]->run_queue;
}

Processor *WorkStealingThreadPool::processor(int worker_id) const {
  if (worker_id < 0 || worker_id >= thread_count_) {
    return nullptr;
  }
  return processors_[static_cast<size_t>(worker_id)].get();
}

bool WorkStealingThreadPool::submit(Task &&task, Processor *hint) {
  if (!task.is_valid()) {
    return false;
  }
  if (thread_count_ <= 0) {
    return false;
  }

  // 优先：投递到本线程绑定的 Processor（本地 runq）。
  if (hint) {
    hint->run_queue.push(std::move(task));
    return true;
  }

  const size_t start = static_cast<size_t>(next_rr());
  const int preferred = stealable_bitmap_.find_non_stealable(start);
  const int target = (preferred >= 0)
                         ? preferred
                         : static_cast<int>(start % static_cast<size_t>(thread_count_));

  processors_[static_cast<size_t>(target)]->run_queue.push(std::move(task));
  return true;
}

void WorkStealingThreadPool::publish_worker_queue(int worker_id) {
  if (worker_id < 0 || worker_id >= thread_count_) {
    start_sem_.post();
    return;
  }

  auto *queue = &processors_[static_cast<size_t>(worker_id)]->run_queue;
  work_queues_[static_cast<size_t>(worker_id)].store(queue,
                                                     std::memory_order_release);

  // 绑定全局位图（H/L 水位，避免每次 push/pop 触发位图写入）。
  // H 要明显大于批处理大小，L 要明显小于 H。
  static constexpr size_t kHighWatermark = 256;
  static constexpr size_t kLowWatermark = 64;
  queue->bind_bitmap(&stealable_bitmap_, worker_id, kHighWatermark,
                     kLowWatermark);

  start_sem_.post();
}

} // namespace zcoroutine
