#ifndef ZLYNX_ZCO_SRC_RUNTIME_TASK_QUEUES_H_
#define ZLYNX_ZCO_SRC_RUNTIME_TASK_QUEUES_H_

#include "zco/detail/wait_queue.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace zco {
namespace detail {
/**
 * @brief 独立于执行栈的共享完成状态，使任务结束后仍可查询和等待。
 *
 * status、exception 和 waiters 均由 mutex 保护；发布终态前先释放任务执行资源。
 */
struct Completion {
    explicit Completion(TaskId identity) : id(identity) {}

    void finish(TaskStatus result, std::exception_ptr error = {});
    TaskId id;
    mutable std::mutex mutex;

    // 仅在 join 需要等待时持锁创建，已完成任务无需分配等待队列。
    std::optional<WaitQueue> waiters;
    TaskStatus status = TaskStatus::pending;
    std::exception_ptr exception;
};

struct PendingTask {
    Task task;
    std::shared_ptr<Completion> completion;
};

/**
 * @brief 将固定任务与可窃取任务分开保存的待执行队列。
 *
 * 队列操作在内部加锁；本线程从队首领取并优先处理固定任务，
 * 其他工作线程只能从可移动队列尾部窃取尚未开始的任务。
 */
class TaskQueues {
  public:
    /**
     * @brief 接管待执行任务，并按是否固定到工作线程选择队列。
     *
     * @param pinned 为 true 时任务只能由所属工作线程领取。
     * @return 插入前两个队列均为空时返回 true，供调用方决定是否唤醒线程。
     */
    bool push(PendingTask task, bool pinned);

    /**
     * @brief 领取一个本地任务，固定任务优先。
     *
     * @param[out] task 成功时接收任务；队列为空时保留原值。
     * @return 是否取得任务。
     */
    bool take(PendingTask &task);

    /**
     * @brief 从可移动队列尾部窃取任务，不触及固定任务。
     *
     * @param[out] task 成功时接收任务；未取得任务时保留原值。
     * @return 是否取得任务。
     */
    bool steal(PendingTask &task);

    size_t size() const { return size_.load(std::memory_order_relaxed); }

    size_t movable_size() const {
        return movable_size_.load(std::memory_order_relaxed);
    }

  private:
    mutable std::mutex mutex_;
    std::deque<PendingTask> pinned_, movable_;

    // 原子计数仅用于负载估计和快速筛选；访问队列内容仍须持 mutex_。
    std::atomic<size_t> size_{0};
    std::atomic<size_t> movable_size_{0};
};
class Worker;

/**
 * @brief 共享的提交准入和空闲线程索引；Executor 仅弱引用此端点。
 *
 * workers 和空闲链表借用 Runtime 拥有的 Worker；提交、停止和销毁
 * 通过 mutex 协调，避免准入检查后工作线程已被销毁。
 */
struct Submission {
    // 这些操作以及工作线程生命周期、提交准入状态均由 mutex 保护。
    void add_idle(Worker *);
    void remove_idle(Worker *);
    void wake_idle();
    std::mutex mutex;
    bool accepting = false;
    std::vector<Worker *> workers;
    size_t next_worker = 0;
    Worker *idle_first = nullptr;
};

Result<TaskHandle> submit(const std::shared_ptr<Submission> &, Task,
                          size_t index, bool pinned);
} // namespace detail
} // namespace zco

#endif
