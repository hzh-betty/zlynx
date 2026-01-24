#ifndef ZCOROUTINE_TASK_QUEUE_H_
#define ZCOROUTINE_TASK_QUEUE_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <queue>

#include "fiber.h"
#include "spinlock.h"

namespace zcoroutine {

/**
 * @brief 任务结构
 * 任务可以是协程或回调函数
 */
struct Task {
  Fiber::ptr fiber;               // 任务协程
  std::function<void()> callback; // 任务回调函数

  Task() = default;

  explicit Task(Fiber::ptr f) : fiber(std::move(f)) {}

  explicit Task(std::function<void()> cb) : callback(std::move(cb)) {}

  // 移动构造函数
  Task(Task &&other) noexcept
      : fiber(std::move(other.fiber)), callback(std::move(other.callback)) {}

  // 移动赋值运算符
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      fiber = std::move(other.fiber);
      callback = std::move(other.callback);
    }
    return *this;
  }

  Task(const Task &) = default;
  Task &operator=(const Task &) = default;

  /**
   * @brief 重置任务
   */
  void reset() {
    fiber = nullptr;
    callback = nullptr;
  }

  /**
   * @brief 任务是否有效
   */
  bool is_valid() const { return fiber != nullptr || callback != nullptr; }
};

/**
 * @brief 任务队列类
 * 使用SpinLock + 条件变量实现的线程安全任务队列
 * 优化要点：
 * 1. 缓存行对齐避免false sharing
 * 2. 批量操作减少锁竞争
 * 3. 快速路径优化：先尝试无锁pop
 * 4. 减少不必要的notify调用：仅当有等待者时才唤醒
 */
class TaskQueue {
public:
  TaskQueue() = default;
  ~TaskQueue() = default;

  /**
   * @brief 添加任务（拷贝版本）
   * @param task 任务对象
   */
  void push(const Task &task);

  /**
   * @brief 添加任务（移动版本，性能优化）
   * @param task 任务对象右值引用
   */
  void push(Task &&task);

  /**
   * @brief 阻塞取出任务（带超时）
   * @param task 输出参数，取出的任务
   * @param timeout_ms 超时时间（毫秒），0表示永久等待
   * @return true表示成功取出，false表示队列已停止或超时
   */
  bool pop(Task &task, int timeout_ms = 0);

  /**
   * @brief 获取队列大小
   * @return 队列中任务数量
   */
  size_t size() const;

  /**
   * @brief 判断队列是否为空
   * @return true表示空，false表示非空
   */
  bool empty() const;

  /**
   * @brief 尝试快速取出任务（无阻塞）
   * @param task 输出参数，取出的任务
   * @return true表示成功取出，false表示队列为空
   */
  bool try_pop(Task &task);

  /**
   * @brief 停止队列
   * 唤醒所有等待的线程
   */
  void stop();

private:
  mutable Spinlock spinlock_; // 自旋锁保护队列
  std::condition_variable_any cv_;        // 条件变量
  std::queue<Task> tasks_;                // 任务队列

  alignas(64) std::atomic<bool> stopped_{false}; // 停止标志
  std::atomic<size_t> size_{0};                  // 原子size，减少锁操作
  std::atomic<int> waiters_{0}; // 等待者计数，减少不必要的notify
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TASK_QUEUE_H_
