/**
 * @file processor.cc
 * @brief processor 实现。
 * @author hzh-betty
 */

#include "zco/internal/processor.h"

#include <errno.h>

#include <chrono>
#include <iterator>
#include <utility>

#include "zco/internal/fiber_stack_manager.h"
#include "zco/internal/io_wait_service.h"
#include "zco/internal/poller.h"
#include "zco/internal/runtime_manager.h"
#include "zco/zco_logger.h"

namespace zco {

// Processor 是“单个调度线程”的核心执行体：
// - 接收 Task 并转化为 Fiber。
// - 维护就绪队列、定时器和 Fiber 上下文切换。
// - 委托 IoWaitService 处理 IO 等待，委托 FiberStackManager 管理共享栈。

namespace {

thread_local Processor *tls_processor = nullptr;

} // namespace

Processor::Processor(int id, size_t stack_size)
    : Processor(id, stack_size, kSharedStackGroupSize, StackModel::kShared) {}

Processor::Processor(int id, size_t stack_size, size_t shared_stack_num,
                     StackModel stack_model)
    : id_(id), stack_size_(stack_size), stack_model_(stack_model),
      running_(false), worker_(), ready_size_(0), ema_loop_ns_(0),
      run_queue_mutex_(), run_queue_(), steal_queue_(), fiber_pool_(4096),
      steal_probe_cursor_(0), timer_queue_(),
      io_(new IoWaitService(id, timer_queue_, create_default_poller(),
                            resume_fiber)),
      stacks_(new FiberStackManager(
          id,
          stack_model == StackModel::kShared
              ? (shared_stack_num == 0 ? 1 : shared_stack_num)
              : 0,
          stack_size)),
      scheduler_context_(), current_fiber_() {}

Processor::~Processor() {
    stop();
    join();

    current_fiber_.reset();
    fiber_pool_.clear();
}

void Processor::start() {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&Processor::run_loop, this);
    ZCO_LOG_INFO("processor started, sched_id={}, stack_size={}", id_,
                 stack_size_);
}

void Processor::stop() {
    running_.store(false, std::memory_order_release);
    wake_loop();
    ZCO_LOG_INFO("processor stop requested, sched_id={}", id_);
}

void Processor::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Processor::enqueue_task(Task task) {
    steal_queue_.push(std::move(task));
    ZCO_LOG_DEBUG("task enqueued, sched_id={}, pending_tasks={}", id_,
                  steal_queue_.size());
    wake_loop();
}

void Processor::enqueue_ready(Fiber::ptr fiber) {
    if (!fiber) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(run_queue_mutex_);
        run_queue_.push_back(std::move(fiber));
        ready_size_.fetch_add(1, std::memory_order_relaxed);
        ZCO_LOG_DEBUG("fiber ready enqueued, sched_id={}, ready_size={}", id_,
                      run_queue_.size());
    }

    wake_loop();
}

void Processor::enqueue_ready_batch(std::deque<Fiber::ptr> *fibers) {
    if (!fibers || fibers->empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(run_queue_mutex_);
        const uint32_t added = static_cast<uint32_t>(fibers->size());
        run_queue_.insert(run_queue_.end(),
                          std::make_move_iterator(fibers->begin()),
                          std::make_move_iterator(fibers->end()));
        fibers->clear();
        ready_size_.fetch_add(added, std::memory_order_relaxed);
        ZCO_LOG_DEBUG(
            "fiber ready batch enqueued, sched_id={}, added={}, ready_size={}",
            id_, added, run_queue_.size());
    }
}

size_t Processor::steal_tasks(std::deque<Task> *tasks, size_t max_steal,
                              size_t min_reserve) {
    const size_t count = steal_queue_.steal(tasks, max_steal, min_reserve);
    ZCO_LOG_DEBUG(
        "tasks stolen from sched_id={}, stolen={}, remaining_tasks={}", id_,
        count, steal_queue_.size());
    return count;
}

uint32_t Processor::pending_task_count() const {
    return static_cast<uint32_t>(steal_queue_.size());
}

int Processor::id() const { return id_; }

Fiber::ptr Processor::current_fiber() const { return current_fiber_; }

ucontext_t *Processor::scheduler_context() { return scheduler_context_.get(); }

void Processor::yield_current() {
    if (!current_fiber_) {
        return;
    }

    current_fiber_->mark_ready();
    Context::swap_context(current_fiber_->context(), &scheduler_context_);
}

void Processor::prepare_wait_current() {
    if (!current_fiber_) {
        return;
    }

    current_fiber_->mark_waiting();
}

bool Processor::park_current() {
    if (!current_fiber_) {
        return false;
    }

    // 切回调度上下文，等待 IO/Timer 或其他路径唤醒后再恢复。
    Context::swap_context(current_fiber_->context(), &scheduler_context_);
    ZCO_LOG_DEBUG(
        "fiber resumed from park, sched_id={}, fiber_id={}, timed_out={}", id_,
        current_fiber_->id(), current_fiber_->timed_out());
    return !current_fiber_->timed_out();
}

bool Processor::park_current_for(uint32_t milliseconds) {
    if (!current_fiber_) {
        return false;
    }

    if (milliseconds == kInfiniteTimeoutMs) {
        return park_current();
    }

    // 为当前等待协程挂一个超时回调，超时后尝试把协程恢复为 ready。
    Fiber::ptr waiting = current_fiber_;
    std::shared_ptr<TimerToken> token =
        add_timer(milliseconds, [waiting]() { resume_fiber(waiting, true); });

    // 协程被正常事件唤醒或超时回调唤醒后都会返回这里。
    const bool ok = park_current();
    token->cancelled.store(true, std::memory_order_release);
    return ok;
}

std::shared_ptr<TimerToken>
Processor::add_timer(uint32_t milliseconds, std::function<void()> callback) {
    std::shared_ptr<TimerToken> token =
        timer_queue_.add_timer(milliseconds, std::move(callback));
    ZCO_LOG_DEBUG("timer added, sched_id={}, delay_ms={}", id_, milliseconds);
    wake_loop();
    return token;
}

bool Processor::wait_fd(int fd, uint32_t events, uint32_t milliseconds) {
    if (!current_fiber_) {
        ZCO_LOG_WARN("wait_fd called without current fiber, sched_id={}, fd={}",
                     id_, fd);
        return false;
    }
    return io_->wait(current_fiber_, fd, events, milliseconds,
                     [this]() { return park_current(); });
}

void Processor::cancel_fd_waiters(int fd, int error) { io_->cancel(fd, error); }

void *Processor::shared_stack_data(size_t stack_slot) {
    return stacks_->data(stack_slot);
}

size_t Processor::shared_stack_size(size_t stack_slot) const {
    return stacks_->size(stack_slot);
}

size_t Processor::shared_stack_count() const { return stacks_->count(); }

StackModel Processor::stack_model() const { return stack_model_; }

uint32_t Processor::queue_load() const {
    return ready_size_.load(std::memory_order_relaxed) +
           static_cast<uint32_t>(steal_queue_.size());
}

uint64_t Processor::load_score() const {
    const uint64_t queue_component =
        static_cast<uint64_t>(queue_load()) * 1000000ULL;
    const uint64_t cpu_component =
        ema_loop_ns_.load(std::memory_order_relaxed) / 1000ULL;
    return queue_component + cpu_component;
}

void Processor::enqueue_stolen_tasks(std::deque<Task> *tasks) {
    steal_queue_.append(tasks);
}

char *Processor::acquire_snapshot_buffer(size_t required_size, size_t *capacity,
                                         uint8_t *bucket_index) {
    return stacks_->acquire_snapshot(required_size, capacity, bucket_index);
}

void Processor::release_snapshot_buffer(char *buffer, uint8_t bucket_index,
                                        size_t capacity) {
    stacks_->release_snapshot(buffer, bucket_index, capacity);
}

void Processor::run_loop() {
    set_current_processor(this);
    ZCO_LOG_INFO("processor loop start, sched_id={}", id_);

    if (!io_->start()) {
        running_.store(false, std::memory_order_release);
    }

    while (running_.load(std::memory_order_acquire)) {
        const auto loop_begin = std::chrono::steady_clock::now();

        // 调度循环按“新任务 -> 就绪协程 -> 定时器 -> 再次就绪”展开。
        // 这样做的目的是让刚入队的工作尽可能在本轮内被执行，减少被 IO
        // 等待和空闲窃取放大的尾延迟。
        drain_new_tasks();
        run_ready_tasks();

        // 定时器放在 ready 之后处理，既保证超时能及时触发，也避免因为
        // 先执行定时器而把本轮刚恢复的协程再次延后到下一轮。
        process_timers();
        run_ready_tasks();

        // 就绪队列持续非空时也检查 I/O，避免读写事件被饿死。
        poll_io_events();
        if (!has_ready_tasks()) {
            steal_tasks_when_idle();
        }

        // 统计本轮循环的 CPU 时间，供负载评分使用。这里的时间包含了
        // run_ready_tasks 中执行任务的时间，因此能反映实际负载。
        const auto loop_end = std::chrono::steady_clock::now();
        const uint64_t loop_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end -
                                                                 loop_begin)
                .count());
        update_load_metrics(loop_ns);
    }

    io_->stop();

    set_current_processor(nullptr);
    ZCO_LOG_INFO("processor loop stop, sched_id={}", id_);
}

bool Processor::has_ready_tasks() const {
    std::lock_guard<std::mutex> lock(run_queue_mutex_);
    return !run_queue_.empty();
}

void Processor::poll_io_events() {
    const int timeout_ms =
        (has_ready_tasks() || pending_task_count() > 0) ? 0 : next_timeout_ms();
    io_->poll(timeout_ms);
}

void Processor::steal_tasks_when_idle() {
    // 空闲时尝试从其他处理器批量窃取待创建任务，提升整体吞吐。
    // 这里先做两个不同起点的探测，再按对方积压量挑一个“更值得偷”的
    // 目标，避免所有空闲线程都盯着同一个 victim。
    const std::vector<std::unique_ptr<Processor>> &all =
        Runtime::instance().processors();
    if (all.size() <= 1) {
        return;
    }

    auto probe_victim = [&](size_t start_offset) -> Processor * {
        const size_t start = (steal_probe_cursor_ + start_offset) % all.size();
        for (size_t step = 0; step < all.size(); ++step) {
            const size_t index = (start + step) % all.size();
            Processor *candidate = all[index].get();
            if (candidate && candidate != this) {
                return candidate;
            }
        }
        return nullptr;
    };

    Processor *victim_a = probe_victim(0);
    Processor *victim_b =
        probe_victim(1 + (steal_probe_cursor_ % (all.size() - 1)));
    steal_probe_cursor_ = (steal_probe_cursor_ + 1) % all.size();

    Processor *chosen = victim_a;
    if (victim_a && victim_b && victim_a != victim_b) {
        const uint32_t a_pending = victim_a->pending_task_count();
        const uint32_t b_pending = victim_b->pending_task_count();
        chosen = (b_pending > a_pending) ? victim_b : victim_a;
    }

    std::deque<Task> stolen_batch;
    // 优先从选中的 victim 批量偷取，失败或数量不足时再尝试备选 victim。
    // 这样能兼顾局部性和负载均衡，减少无谓的全局扫描。
    if (chosen && chosen->steal_tasks(&stolen_batch, 64, 2) > 0) {
    } else if (victim_b && victim_b != chosen &&
               victim_b->steal_tasks(&stolen_batch, 64, 2) > 0) {
    }

    if (stolen_batch.empty()) {
        return;
    }

    ZCO_LOG_DEBUG("tasks stolen by scheduler, thief_sched_id={}, stolen={}",
                  id_, stolen_batch.size());
    enqueue_stolen_tasks(&stolen_batch);
}

void Processor::update_load_metrics(uint64_t loop_ns) {
    uint64_t old_ema = ema_loop_ns_.load(std::memory_order_relaxed);
    while (true) {
        const uint64_t new_ema =
            (old_ema == 0) ? loop_ns : ((old_ema * 7ULL + loop_ns) >> 3);
        if (ema_loop_ns_.compare_exchange_weak(old_ema, new_ema,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
            break;
        }
    }
}

void Processor::wake_loop() { io_->wake(); }

void Processor::drain_new_tasks() {
    constexpr size_t kTaskMaterializeBatchLimit = 256;

    std::deque<Task> pending;
    steal_queue_.drain_some(&pending, kTaskMaterializeBatchLimit);
    if (pending.empty()) {
        return;
    }

    std::deque<Fiber::ptr> ready_batch;

    // Task 只在调度线程里实体化成 Fiber，这样可以把上下文初始化、共享
    // 栈槽位分配和 Fiber 注册都限制在单线程内完成，避免跨线程构造复杂
    // 的协程上下文。
    while (!pending.empty()) {
        Task task = std::move(pending.front());
        pending.pop_front();
        Fiber::ptr fiber = obtain_fiber(std::move(task));
        ZCO_LOG_DEBUG("task materialized to fiber, sched_id={}, fiber_id={}",
                      id_, fiber->id());
        ready_batch.push_back(std::move(fiber));
    }

    enqueue_ready_batch(&ready_batch);
}

void Processor::run_ready_tasks() {
    constexpr size_t kReadyDispatchBatchLimit = 256;

    std::deque<Fiber::ptr> ready_batch;
    // 每次最多执行一批；主动 yield 的协程留给下一轮。
    if (drain_ready_fibers(&ready_batch, kReadyDispatchBatchLimit) > 0) {
        while (!ready_batch.empty()) {
            Fiber::ptr fiber = std::move(ready_batch.front());
            ready_batch.pop_front();
            if (recycle_if_done_before_run(fiber)) {
                continue;
            }

            // Fiber 从 ready 队列进入运行态后，只有两种结局：主动让出后
            // 重新回到 ready，或者执行完成后回收。中间状态都在切换回来后
            // 统一收口。
            Fiber::ptr resumed = switch_to_fiber(std::move(fiber));
            const Fiber::State state = finalize_after_switch(resumed);
            dispatch_resumed_fiber(std::move(resumed), state);
        }
    }
}

size_t Processor::drain_ready_fibers(std::deque<Fiber::ptr> *fibers,
                                     size_t max_count) {
    if (!fibers || max_count == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(run_queue_mutex_);
    if (run_queue_.empty()) {
        return 0;
    }

    const size_t drained = std::min(max_count, run_queue_.size());
    for (size_t i = 0; i < drained; ++i) {
        fibers->push_back(std::move(run_queue_.front()));
        run_queue_.pop_front();
    }
    ready_size_.fetch_sub(static_cast<uint32_t>(drained),
                          std::memory_order_relaxed);
    return drained;
}

bool Processor::recycle_if_done_before_run(const Fiber::ptr &fiber) {
    if (fiber->state() != Fiber::State::kDone) {
        return false;
    }

    ZCO_LOG_DEBUG("skip done fiber before run, sched_id={}, fiber_id={}", id_,
                  fiber->id());
    Runtime::instance().unregister_fiber(fiber.get());
    recycle_fiber(fiber);
    return true;
}

Fiber::ptr Processor::switch_to_fiber(Fiber::ptr fiber) {
    current_fiber_ = std::move(fiber);

    if (!current_fiber_->context_initialized()) {
        // 首次运行需要初始化 ucontext；后续恢复只做栈快照回填。
        current_fiber_->initialize_context();
    }
    stacks_->prepare(current_fiber_);

    current_fiber_->mark_running();
    Context *fiber_context = current_fiber_->context();
    ZCO_LOG_DEBUG("switch to fiber, sched_id={}, fiber_id={}", id_,
                  current_fiber_->id());
    Context::swap_context(&scheduler_context_, fiber_context);

    Fiber::ptr resumed = current_fiber_;
    current_fiber_.reset();
    return resumed;
}

Fiber::State Processor::finalize_after_switch(const Fiber::ptr &fiber) {
    const Fiber::State state = fiber->state();
    if (state == Fiber::State::kDone) {
        stacks_->release(fiber);
    }
    return state;
}

void Processor::dispatch_resumed_fiber(Fiber::ptr fiber, Fiber::State state) {
    if (state == Fiber::State::kReady) {
        // 主动 yield 或被唤醒后进入 ready，重新排队等待下一轮调度。
        enqueue_ready(std::move(fiber));
        return;
    }

    if (state != Fiber::State::kDone) {
        return;
    }

    ZCO_LOG_DEBUG("fiber completed and unregistered, sched_id={}, fiber_id={}",
                  id_, fiber->id());
    Runtime::instance().unregister_fiber(fiber.get());
    recycle_fiber(fiber);
}

void Processor::process_timers() { timer_queue_.process_due(); }

int Processor::next_timeout_ms() const {
    return timer_queue_.next_timeout_ms();
}

Fiber::ptr Processor::obtain_fiber(Task task) {
    size_t stack_slot = 0;
    if (stack_model_ == StackModel::kShared) {
        stack_slot = stacks_->next_slot();
    }

    const int fiber_id = Runtime::instance().next_fiber_id();

    Fiber::ptr fiber = fiber_pool_.acquire();

    if (fiber) {
        fiber->reset(fiber_id, std::move(task), stack_slot);
        return fiber;
    }

    // Task -> Fiber 的“实体化”发生在调度线程，避免跨线程创建上下文。
    return std::make_shared<Fiber>(fiber_id, this, std::move(task), stack_size_,
                                   stack_slot,
                                   stack_model_ == StackModel::kShared);
}

void Processor::recycle_fiber(const Fiber::ptr &fiber) {
    fiber_pool_.recycle(fiber);
}

Processor *current_processor() { return tls_processor; }

void set_current_processor(Processor *processor) { tls_processor = processor; }

} // namespace zco
