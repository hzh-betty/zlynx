#include "runtime/worker.h"
#include <algorithm>
#include <limits>

namespace zco {
namespace detail {
thread_local Worker *current_worker = nullptr;

void Inbox::notify(WaitId id) {
    bool empty;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        empty = notifications_.empty();
        notifications_.push_back(id);
    }
    if (empty)
        if (auto reactor = reactor_.lock())
            reactor->wake();
}

std::vector<WaitId> Inbox::drain() {
    std::vector<WaitId> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.swap(notifications_);
    return result;
}

Worker::Worker(size_t index, const RuntimeOptions &options,
               std::weak_ptr<Submission> submission,
               std::shared_ptr<Reactor> reactor)
    : index_(index), steal_cursor_(index + 1),
      submission_(std::move(submission)), reactor_(std::move(reactor)),
      inbox_(std::make_shared<Inbox>(reactor_)), stacks_(options),
      io_waits_(reactor_) {}

Worker::~Worker() {
    if (thread_.joinable()) {
        stop();
        join();
    }
}

void Worker::start(const std::shared_ptr<Startup> &startup) {
    thread_ = std::thread([this, startup] {
        {
            std::unique_lock<std::mutex> lock(startup->mutex);
            ++startup->ready;
            startup->cv.notify_all();
            startup->cv.wait(lock,
                             [&] { return startup->launch || startup->abort; });
            if (startup->abort)
                return;
        }
        current_worker = this;
        run();
        current_worker = nullptr;
    });
}

/**
    stop()后下一轮run：
    取消待执行队列中的任务
    → 尝试取消已挂起的等待
    → 恢复协程
    → 让业务处理取消并退出
    → 删除执行记录
    → tasks_为空时结束
*/
void Worker::stop() {
    inbox_->stopping = true;
    reactor_->wake();
}

void Worker::join() {
    if (thread_.joinable())
        thread_.join();
}

bool Worker::push(PendingTask task, bool pinned) {
    bool empty = queues_.push(std::move(task), pinned);
    if (empty)
        reactor_->wake();
    return empty;
}

TaskId Worker::current_id() const {
    return current_ ? current_->pending.completion->id : TaskId{};
}

void Worker::entry() {
    auto *worker = current_worker;
    auto *record = worker->current_;
    try {
        record->pending.task();
    } catch (const TaskCanceled &) {
        record->result = TaskStatus::canceled;
    } catch (...) {
        record->exception = std::current_exception();
        record->result = TaskStatus::failed;
    }
    record->phase = Phase::finished;
    record->execution->suspend(worker->caller_);
    std::terminate();
}

/* 协程栈：
用户函数
  → yield_current / park / entry收尾
    → Continuation::suspend
      → 切回调度器
*/
void Worker::yield_current() {
    if (inbox_->stopping) // 任务在 yield 前被取消，抛出异常让用户函数展开资源。
        throw TaskCanceled();
    current_->phase = Phase::ready;
    current_->execution->suspend(caller_);
    if (inbox_->stopping) 
        throw TaskCanceled(); // 任务在 yield 后被取消，抛出异常让用户函数展开资源。
}

std::shared_ptr<WaitState> Worker::make_wait() {
    if (!current_)
        throw std::logic_error("Wait without a running task");
    return std::make_shared<WaitState>(
        // 给每次等待分配独立身份。
        WaitId{current_id(), ++current_->generation}, inbox_);
}

WaitOutcome Worker::park(const std::shared_ptr<WaitState> &wait,
                         Deadline deadline) {
    // 停止和超时都只是尝试完成等待。因为 WaitState 只接受第一次完成，
    // 如果 ready 已经先发布，就不会被覆盖。
    if (inbox_->stopping)
        wait->complete(WaitOutcome::canceled);
    if (deadline.expired(Deadline::Clock::now()))
        wait->complete(WaitOutcome::timeout);

    if (wait->completed()) // 先检查是否已完成，避免在切换过程中丢失唤醒。
        return wait->outcome();
    if (wait->id().task != current_id())
        throw std::logic_error("Wait belongs to a different task");

    // 先注册定时器，再挂起当前任务；挂起期间可能被唤醒或超时。
    auto timer = timers_.add(deadline, WakeToken(wait));
    current_->wait = wait;
    current_->phase = Phase::waiting;
    try {
        current_->execution->suspend(caller_);
    } catch (...) {
        timers_.cancel(timer);
        current_->wait.reset();
        throw;
    }
    timers_.cancel(timer);
    current_->wait.reset();
    return wait->outcome();
}

Result<void>
Worker::wait_io(const std::shared_ptr<io::detail::Resource> &resource,
                io::Interest interest, Deadline deadline) {
    // 创建等待状态
    auto wait = make_wait();
    
    // 登记I/O等待
    auto registration = io_waits_.add(wait, resource, interest);
    if (!registration)
        return Result<void>(registration.error());
    
    //挂起并等待结果
    auto outcome = park(wait, deadline);

    // 恢复后撤销登记
    auto removed = registration.value().finish();
    return Result<void>(outcome == WaitOutcome::ready ? removed.error()
                                                      : wait_error(outcome));
}

bool Worker::take_task(PendingTask &task) {
    if (queues_.take(task))
        return true;
    auto start = steal_cursor_++;
    for (size_t offset = 0; offset < peers_.size(); ++offset) {
        auto *peer = peers_[(start + offset) % peers_.size()];
        if (peer != this && peer->queues_.steal(task))
            return true;
    }
    return false;
}

void Worker::cancel_pending() {
    PendingTask pending;
    while (queues_.take(pending)) {
        pending.task = {};
        pending.completion->finish(TaskStatus::canceled);
    }
}

void Worker::collect_notifications() {
    // 把有效通知转换成就绪任务。
    for (auto id : inbox_->drain()) {
        auto found = tasks_.find(id.task.value);
        if (found == tasks_.end())
            continue;
        auto &record = *found->second;

        // 同一任务可能已开始下一次等待；只有代次匹配的完成通知才能重新入队。
        if (record.phase == Phase::waiting && record.wait &&
            record.wait->id() == id && record.wait->completed()) {
            // 已经入队的任务不能被同类通知重复入队 Waiting->Ready，避免无限循环。
            // resume 有条路径也可能入队
            record.phase = Phase::ready;
            ready_.push_back(id.task.value);
        }
    }
}

/**
 * 调度器栈：
 * run
 *   → run_batch
 *     → Worker::resume
 *       → Continuation::resume
 *         → 切入协程
 */
void Worker::resume(Record &record) {
    current_ = &record;
    record.phase = Phase::running;
    try {
        if (!record.execution)
            record.execution.reset(new Continuation(stacks_, entry));
        record.execution->resume(caller_);

        // 快照分配失败时，原共享栈仍完好；恢复任务并在挂起点抛出异常，
        // 让任务栈上的资源正常展开后再销毁执行记录。
        /*
            Worker 选中任务
                ↓
            首次执行时创建 Continuation
                ↓
            Continuation.resume()
                ↓
            任务执行
                ↓
            任务 yield / 等待 / 结束
                ↓
            Continuation.suspend()
                ↓
            回到 Worker
                ↓
            未结束：save()
            已结束：销毁任务记录
        */
        while (record.phase != Phase::finished) {
            try {
                /*
                    保存失败
                    → 原共享栈尚未复用，内容仍完整
                    → 直接恢复原栈
                    → suspend() 返回时抛出保存失败的异常
                    → 异常在任务调用栈中传播
                    → 局部对象正常析构
                    → entry捕获异常并结束
                */
                record.execution->save();
                break;
            } catch (...) {
                record.phase = Phase::running;
                record.execution->interrupt(caller_, std::current_exception());
            }
        }
    } catch (...) {
        // 切换栈前发生失败时，不发布尚未完成初始化的执行状态。
        record.result = TaskStatus::failed;
        record.exception = std::current_exception();
        record.phase = Phase::finished;
    }

    // 回到调度器栈后判断任务状态，发布完成或重新入队
    current_ = nullptr;
    auto id = record.pending.completion->id.value;
    if (record.phase == Phase::finished) {
        auto completion = record.pending.completion;
        auto result = record.result;
        auto exception = record.exception;
        record.pending.task = {};
        tasks_.erase(
            id); // 先释放执行资源再发布完成，确保等待完成的一方不会提前销毁关联资源。
        --live_;
        completion->finish(result, exception);
    } else if (record.phase == Phase::ready ||
               (record.wait && record.wait->completed())) { // 如果通知发生在检查之后、切换期间，再由调度器检查, 防止丢失唤醒。
        record.phase = Phase::ready;
        ready_.push_back(id);
    }
}

void Worker::run_batch() {
    for (size_t budget = 0; budget < 64; ++budget) {
        if (!ready_.empty() && (inbox_->stopping || budget % 2)) {
            //  恢复已有就绪任务
            auto id = ready_.front();
            ready_.pop_front();
            auto found = tasks_.find(id);
            if (found != tasks_.end())
                resume(*found->second);
        } else {
            // 领取新任务
            if (inbox_->stopping)
                break;
            PendingTask pending;

            // 领取本地或窃取任务；若没有任务则恢复就绪队列中的任务
            if (!take_task(pending)) {
                if (ready_.empty())
                    break;
                auto id = ready_.front();
                ready_.pop_front();
                resume(*tasks_.at(id));
                continue;
            }

            // 领取到任务后，必须在恢复前检查运行时是否已停止，避免在恢复后才发现运行时已停止。
            if (inbox_->stopping) {
                pending.task = {};
                pending.completion->finish(TaskStatus::canceled);
                continue;
            }
            auto completion = pending.completion;
            try {
                // 任务已领取，建立执行记录并恢复
                auto record = std::unique_ptr<Record>(new Record());
                record->pending = std::move(pending);
                auto id = completion->id.value;
                auto inserted = tasks_.emplace(id, std::move(record));
                ++live_;
                {
                    std::lock_guard<std::mutex> lock(completion->mutex);
                    completion->status = TaskStatus::running;
                }
                resume(*inserted.first->second);
            } catch (...) {
                pending.task = {};
                completion->finish(TaskStatus::failed,
                                   std::current_exception());
            }
        }
    }
}

int Worker::poll_timeout() const {
    int timeout = 0;
    if (ready_.empty() && queues_.size() == 0) {
        auto deadline = timers_.next_deadline();
        timeout = -1;
        if (!deadline.is_infinite()) {
            auto duration = deadline.time() - Deadline::Clock::now();
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                    .count();
            timeout = static_cast<int>(std::max<int64_t>(
                0, std::min<int64_t>(ms + 1, std::numeric_limits<int>::max())));
        }
    }
    return timeout;
}

bool Worker::poll_once(int timeout) {
    bool registered_idle = false;
    if (timeout) {
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);

            // 提交准入和空闲登记共用同一把锁；登记前已发布的任务
            // 也必须阻止工作线程休眠，避免错过唤醒。
            bool pending = inbox_->stopping || queues_.size() != 0;
            for (auto *worker : endpoint->workers)
                if (worker->queues_.movable_size()) { // 其他线程有可窃取任务
                    pending = true;
                    break;
                }
            if (pending)
                timeout = 0;
            else {
                endpoint->add_idle(this);
                registered_idle = true;
            }
        }
    }
    bool succeeded = true;
    try {
        for (auto event : reactor_->poll(timeout))
            io_waits_.dispatch(event);
    } catch (...) {
        succeeded = false;
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);
            endpoint->accepting = false;
            for (auto *worker : endpoint->workers)
                worker->stop();
        } else
            stop();
    }
    if (registered_idle)
        if (auto endpoint = submission_.lock()) {
            std::lock_guard<std::mutex> lock(endpoint->mutex);
            endpoint->remove_idle(this);
        }
    return succeeded;
}

void Worker::run() {
    bool reactor_failed = false;
    while (true) {
        // 如果
        if (inbox_->stopping) {
            cancel_pending();
            for (auto &entry : tasks_)
                if (entry.second->wait)
                    entry.second->wait->complete(WaitOutcome::canceled);
        }

        // 处理到期定时器
        timers_.expire(Deadline::Clock::now());
        
        // 收集完成通知
        collect_notifications();

        // 运行一批任务
        run_batch();

        if (inbox_->stopping && tasks_.empty())
            break;
        if (!reactor_failed)
            reactor_failed = !poll_once(poll_timeout());
    }
    cancel_pending();
}
} // namespace detail
} // namespace zco
