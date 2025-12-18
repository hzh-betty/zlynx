#include "scheduling/scheduler.h"
#include "util/thread_context.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

Scheduler::Scheduler(int thread_count, const std::string& name)
    : name_(name)
    , thread_count_(thread_count)
    , task_queue_(std::make_unique<TaskQueue>())
    , stopping_(false)
    , active_thread_count_(0)
    , idle_thread_count_(0) {
    
    ZCOROUTINE_LOG_INFO("Scheduler[{}] created with thread_count={}", name_, thread_count_);
}

Scheduler::~Scheduler() {
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] destroying", name_);
    stop();
    ZCOROUTINE_LOG_INFO("Scheduler[{}] destroyed", name_);
}

void Scheduler::start() {
    if (!threads_.empty()) {
        ZCOROUTINE_LOG_WARN("Scheduler[{}] already started, skip", name_);
        return;
    }
    
    ZCOROUTINE_LOG_INFO("Scheduler[{}] starting with {} threads...", name_, thread_count_);
    
    // 创建工作线程
    threads_.reserve(thread_count_);
    for (int i = 0; i < thread_count_; ++i) {
        auto thread = std::make_unique<std::thread>([this, i]() {
            // 设置线程的调度器
            ThreadContext::SetScheduler(this);
            
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} started", name_, i);
            this->run();
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} exited", name_, i);
        });
        threads_.push_back(std::move(thread));
    }
    
    ZCOROUTINE_LOG_INFO("Scheduler[{}] started successfully with {} threads", name_, thread_count_);
}

void Scheduler::stop() {
    if (stopping_.exchange(true, std::memory_order_relaxed)) {
        ZCOROUTINE_LOG_DEBUG("Scheduler[{}] already stopping, skip", name_);
        return;  // 已经在停止中
    }
    
    ZCOROUTINE_LOG_INFO("Scheduler[{}] stopping, active_threads={}, pending_tasks={}", 
                        name_, active_thread_count_.load(), task_queue_->size());
    
    // 停止任务队列，唤醒所有等待的线程
    task_queue_->stop();
    
    // 等待所有线程结束
    for (size_t i = 0; i < threads_.size(); ++i) {
        auto& thread = threads_[i];
        if (thread && thread->joinable()) {
            thread->join();
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} joined", name_, i);
        }
    }
    threads_.clear();
    
    ZCOROUTINE_LOG_INFO("Scheduler[{}] stopped successfully", name_);
}

void Scheduler::schedule(Fiber::ptr fiber) {
    if (!fiber) {
        ZCOROUTINE_LOG_WARN("Scheduler[{}]::schedule received null fiber", name_);
        return;
    }
    
    Task task(fiber);
    task_queue_->push(task);
    
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] scheduled fiber name={}, id={}, queue_size={}",
                         name_, fiber->name(), fiber->id(), task_queue_->size());
}

void Scheduler::schedule(std::function<void()> func) {
    if (!func) {
        ZCOROUTINE_LOG_WARN("Scheduler[{}]::schedule received null callback", name_);
        return;
    }
    
    Task task(func);
    task_queue_->push(task);
    
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] scheduled callback, queue_size={}", name_, task_queue_->size());
}

void Scheduler::set_fiber_pool(FiberPool::ptr pool) {
    fiber_pool_ = pool;
    ZCOROUTINE_LOG_INFO("Scheduler[{}] fiber pool configured", name_);
}

Scheduler* Scheduler::get_this() {
    return ThreadContext::GetScheduler();
}

void Scheduler::set_this(Scheduler* scheduler) {
    ThreadContext::SetScheduler(scheduler);
}

void Scheduler::run() {
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread entering run loop", name_);
    
    while (!stopping_.load(std::memory_order_relaxed)) {
        Task task;
        
        // 从队列中取出任务（阻塞等待）
        if (!task_queue_->pop(task)) {
            // 队列已停止
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] task queue stopped, exiting run loop", name_);
            break;
        }
        
        if (!task.is_valid()) {
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] received invalid task, skipping", name_);
            continue;
        }
        
        // 增加活跃线程计数
        int active = active_thread_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        
        // 执行任务
        if (task.fiber) {
            // 执行协程
            Fiber::ptr fiber = task.fiber;
            
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] executing fiber name={}, id={}, active_threads={}",
                                 name_, fiber->name(), fiber->id(), active);
            
            try {
                fiber->resume();
            } catch (const std::exception& e) {
                ZCOROUTINE_LOG_ERROR("Scheduler[{}] fiber execution exception: name={}, id={}, error={}",
                                     name_, fiber->name(), fiber->id(), e.what());
            } catch (...) {
                ZCOROUTINE_LOG_ERROR("Scheduler[{}] fiber execution unknown exception: name={}, id={}",
                                     name_, fiber->name(), fiber->id());
            }
            
            // 如果协程终止且有协程池，归还到池中
            if (fiber->state() == Fiber::State::kTerminated) {
                ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber terminated: name={}, id={}",
                                     name_, fiber->name(), fiber->id());
                if (fiber_pool_) {
                    fiber_pool_->release(fiber);
                }
            }
        } else if (task.callback) {
            // 执行回调函数
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] executing callback, active_threads={}", name_, active);
            
            try {
                task.callback();
            } catch (const std::exception& e) {
                ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback exception: error={}", name_, e.what());
            } catch (...) {
                ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback unknown exception", name_);
            }
        }
        
        // 减少活跃线程计数
        active_thread_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread exiting run loop", name_);
}

} // namespace zcoroutine
