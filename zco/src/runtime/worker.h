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
 * @brief Inbox 把来自定时器、I/O、同步对象、
    关闭操作或其他线程的等待完成事件，安全地转交给负责该协程的 Worker，
    并让 Worker 在自己的线程中恢复协程。
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

    /**
     * @brief 将当前任务挂起，直到指定的等待条件满足。
     *
     * @param wait 等待状态
     * @param deadline 超时时间
     * @return 等待结果
     */
    WaitOutcome park(const std::shared_ptr<WaitState> &, Deadline);
    
    Result<void> wait_io(const std::shared_ptr<io::detail::Resource> &,
                         io::Interest, Deadline);

  private:
    friend struct Submission;
    /*
    * Worker::Phase 状态转换图（执行记录的内部调度状态）
    *
    *                         +-----------+
    *              +--------->|  running  |----------+
    *              |          +-----------+          |
    *              |                |                |
    *            resume            park         返回 / 异常退出
    *              |                |                |
    *              |                v                v
    *          +-------+       +---------+      +----------+
    *          | ready |<------| waiting |      | finished |
    *          +-------+       +---------+      +----------+
    *              ^            等待完成              |
    *              |                                  v
    *              |                             释放执行记录
    *              |
    *              +---- running 经 yield 主动让出执行权
    *
    * 状态说明：
    *   ready    : 就绪，等待调度执行
    *   running  : 当前处于执行阶段，正在运行或准备切入任务上下文
    *   waiting  : 已设置挂起等待，等待 WaitState 完成后重新就绪
    *   finished : 执行结束，不再调度
    *
    * 常规转换：
    *   创建 Record -> ready
    *   ready   -> running  : resume 选中并执行任务
    *   running -> ready    : yield 主动让出执行权
    *   running -> waiting  : park 等待尚未完成，设置状态并切回调度器
    *   waiting -> ready    : 匹配的完成通知到达，或切回调度器时等待已完成
    *   running -> finished : 正常返回、异常退出或执行初始化失败
    *   finished -> 释放记录 : 释放执行资源，再发布任务完成结果
    *
    * 特殊路径：
    *   running -> running : park 发现等待已完成，直接返回
    *   ready   -> running : 快照保存失败，interrupt 恢复原栈并注入异常
    *   waiting -> running : 快照保存失败，interrupt 恢复原栈并注入异常
    *
    * 注意：
    *   正常唤醒经过 waiting -> ready -> running。
    *   完成通知校验 phase、任务 ID 和等待代次，避免旧通知重复入队。
    *   ready、running、waiting 均对应对外 TaskStatus::running。
    */
    enum class Phase { running, ready, waiting, finished };

    struct Record {
        PendingTask pending; // 用户函数和共享完成状态。
        std::unique_ptr<Continuation> execution; // 协程上下文与栈的管理对象。
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

    /**
      @brief 计算下一次 poll 的超时时间。
      有就绪任务或待执行任务	0
      没有可运行任务，有有限定时器	距最近截止时间的毫秒数加 1
      没有可运行任务，也没有有限定时器	-1 
    */
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
    TaskQueues queues_; // 任务还没开始，没有建立执行上下文
    std::shared_ptr<Reactor> reactor_;
    std::shared_ptr<Inbox> inbox_; // 用于接收外部通知
    StackArena stacks_;
    NativeContext caller_; // 调度器上下文，切入任务前保存调度器栈，切回时恢复调度器栈
    std::unordered_map<uint64_t, std::unique_ptr<Record>> tasks_; //任务已经开始，拥有执行记录可能正在执行、可以恢复，或者正在等待
    std::deque<uint64_t> ready_;  // tasks_ 中当前可以执行的任务 ID
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
