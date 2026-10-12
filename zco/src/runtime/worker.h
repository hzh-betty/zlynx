#ifndef ZLYNX_ZCO_SRC_RUNTIME_WORKER_H_
#define ZLYNX_ZCO_SRC_RUNTIME_WORKER_H_

#include "execution/continuation.h"
#include "io/wait_registry.h"
#include "runtime/task_queues.h"
#include "wait/timer_queue.h"
#include "wait/wait_state.h"
#include <condition_variable>
#include <thread>
#include <unordered_map>

namespace zco {
namespace detail {
/**
 * @brief 工作线程启动屏障，等待全部线程就绪后统一进入调度。
 *
 * ready、launch 和 abort 均由 mutex 保护；启动失败时通过 abort 释放已启动线程。
 */
struct Startup {
    std::mutex mutex;
    std::condition_variable cv;
    size_t ready = 0;
    bool launch = false, abort = false;
};

/**
 * @brief 接收跨线程完成通知的邮箱，按批交给所属工作线程处理。
 *
 * 空到非空的转换唤醒反应器；通知只携带等待 ID，不持有任务或执行栈。
 */
class Inbox final : public CompletionEndpoint {
  public:
    explicit Inbox(std::weak_ptr<Reactor> reactor)
        : reactor_(std::move(reactor)) {}

    void notify(WaitId id) override;
    std::vector<WaitId> drain();
    std::atomic<bool> stopping{false};

  private:
    std::mutex mutex_;
    std::vector<WaitId> notifications_;
    std::weak_ptr<Reactor> reactor_;
};

class TaskCanceled : public std::exception {};

/**
 * @brief 将待执行任务、协程栈、定时器和 I/O 事件串联的单线程调度器。
 *
 * tasks_、ready_、timers_ 和当前执行状态仅由所属线程访问；
 * 外部提交与完成通知分别通过内部加锁的 TaskQueues 和 Inbox 进入。
 */
class Worker {
  public:
    Worker(size_t index, const RuntimeOptions &, std::weak_ptr<Submission>,
           std::shared_ptr<Reactor>);
    ~Worker();
    void start(const std::shared_ptr<Startup> &);
    void stop();

    void wake() { reactor_->wake(); }

    void join();
    bool push(PendingTask task, bool pinned);

    // 调用方须持有提交端点的互斥量。
    bool idle() const { return idle_; }

    size_t load() const {
        return queues_.size() + live_.load(std::memory_order_relaxed);
    }

    void set_peers(std::vector<Worker *> peers) { peers_ = std::move(peers); }

    size_t index() const { return index_; }

    std::weak_ptr<Submission> submission() const { return submission_; }

    TaskId current_id() const;
    void yield_current();
    std::shared_ptr<WaitState> make_wait();
    WaitOutcome park(const std::shared_ptr<WaitState> &, Deadline);
    Result<void> wait_io(const std::shared_ptr<io::detail::Resource> &,
                         io::Interest, Deadline);

  private:
    friend struct Submission;
    enum class Phase { running, ready, waiting, finished };

    struct Record {
        PendingTask pending;
        std::unique_ptr<Continuation> execution;
        Phase phase = Phase::ready;
        std::shared_ptr<WaitState> wait;

        // 每次等待递增；配合任务 ID 过滤已超时或已结束的旧通知。
        uint64_t generation = 0;
        TaskStatus result = TaskStatus::succeeded;
        std::exception_ptr exception;
    };

    static void entry();
    void run();
    void run_batch();
    int poll_timeout() const;
    bool poll_once(int timeout);
    bool take_task(PendingTask &);
    void collect_notifications();
    void resume(Record &);
    void cancel_pending();
    size_t index_;
    size_t steal_cursor_;
    std::weak_ptr<Submission> submission_;
    std::vector<Worker *> peers_;
    TaskQueues queues_;
    std::shared_ptr<Reactor> reactor_;
    std::shared_ptr<Inbox> inbox_;
    StackArena stacks_;
    NativeContext caller_;
    std::unordered_map<uint64_t, std::unique_ptr<Record>> tasks_;
    std::deque<uint64_t> ready_;
    IoWaitRegistry io_waits_;
    TimerQueue timers_;
    Record *current_ = nullptr;

    // 已建立执行记录的任务数；与队列数量共同用于负载估计。
    std::atomic<size_t> live_{0};

    // 借用的空闲链表节点，仅在持有 Submission::mutex 时访问。
    Worker *idle_next_ = nullptr, *idle_previous_ = nullptr;
    bool idle_ = false;
    std::thread thread_;
};

extern thread_local Worker *current_worker;
} // namespace detail
} // namespace zco

#endif
