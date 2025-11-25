#include "scheduler.h"

#include <unistd.h>
#include <sys/eventfd.h>

#include <stdexcept>
#include <utility>

#include "zlynx_logger.h"

namespace zlynx
{
    static thread_local Scheduler *t_scheduler = nullptr; // 当前线程的调度器指针
    static thread_local Fiber *t_scheduler_fiber = nullptr; // 当前线程的调度器协程指针

    Scheduler::Scheduler(int thread_count, bool use_caller, std::string name)
        : name_(std::move(name)), use_caller_(use_caller)
    {
        if (thread_count <= 0) thread_count = 1;
        if(use_caller_)
        {
            thread_count-= 1;
            Fiber::get_fiber();
            t_scheduler = this;
            root_fiber_ = std::make_unique<Fiber>([this](){ run();});
            t_scheduler_fiber = root_fiber_.get();
        }
        total_thread_count_ = thread_count;
        tickle_id = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (tickle_id == -1)
        {
            throw std::runtime_error("eventfd failed");
        }
    }

    Scheduler::~Scheduler()
    {
        stop();
        if (tickle_id != -1)
        {
            close(tickle_id);
        }
    }

    void Scheduler::start()
    {
        MutexType::Lock lock(mutex_);
        if (stopping_) return;

        threads_.reserve(total_thread_count_);
        for (size_t i = 0; i < total_thread_count_; ++i)
        {
            // 创建线程并绑定到调度器的run方法
            auto ptr = std::make_unique<Thread>([this]()
            {
                t_scheduler = this;
                const auto fiber = Fiber::get_fiber();
                t_scheduler_fiber = fiber;
                this->run();
            }, name_ + "_" + std::to_string(i));
            threads_.emplace_back(std::move(ptr));
        }

        if (use_caller_)
        {
            t_scheduler = this;
        }
    }

    void Scheduler::stop()
    {
        stopping_ = true;

        for (size_t i = 0; i < threads_.size(); ++i)
        {
            tickle();
        }

        for (const auto& ptr : threads_)
        {
            ptr->join();
        }
        threads_.clear();

        if (use_caller_)
        {
            if (get_this() == this && root_fiber_)
            {
                tickle();
            }
        }
    }

    bool Scheduler::is_stop() const
    {
        return stopping_.load() && tasks_.empty() && active_thread_count_.load() == 0;
    }

    Scheduler *Scheduler::get_this()
    {
        return t_scheduler;
    }

    Fiber *Scheduler::get_scheduler_fiber()
    {
        return t_scheduler_fiber;
    }

    void Scheduler::run() noexcept
    {
        ZLYNX_LOG_DEBUG("[{}] thread start",zlynx::Thread::get_name());
        Fiber::ptr idle_fiber = std::make_unique<Fiber>([this]()
        {
            idle();
        });
        Fiber::ptr cb_fiber; // 用于执行回调的协程

        while (true)
        {
            Task task;
            bool have_task = false;
            bool need_tickle = false;

            {
                MutexType::Lock lock(mutex_);
                if (!tasks_.empty())
                {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                    ++active_thread_count_;
                    have_task = true;
                }
                need_tickle = (!tasks_.empty()) && (idle_thread_count_ > 0);
            }

            if (need_tickle)
            {
                tickle();
            }

            if (have_task)
            {
                if (task.fiber && task.fiber->state() != Fiber::State::kTerminated)
                {
                    try
                    {
                        task.fiber->resume();
                    }
                    catch (const std::exception &e)
                    {
                        ZLYNX_LOG_ERROR("Fiber exception: {}", e.what());
                    }
                    catch (...)
                    {
                        ZLYNX_LOG_ERROR("Fiber unknown exception");
                    }
                    --active_thread_count_;
                    if (task.fiber->state() != Fiber::State::kTerminated)
                    {
                        schedule(std::move(task.fiber));
                    }
                    task.reset();
                }
                else if (task.callback)
                {
                    if (cb_fiber)
                    {
                        cb_fiber->reset(std::move(task.callback));
                    }
                    else
                    {
                        cb_fiber = std::make_unique<Fiber>(std::move(task.callback));
                    }
                    task.reset();

                    try
                    {
                        cb_fiber->resume();
                    }
                    catch (const std::exception &e)
                    {
                        ZLYNX_LOG_ERROR("Fiber exception: {}", e.what());
                    }
                    catch (...)
                    {
                        ZLYNX_LOG_ERROR("Fiber unknown exception");
                    }
                }
                else
                {
                    ZLYNX_LOG_ERROR("No fiber or callback!");
                    --active_thread_count_;
                }

            }
            else
            {
               if (is_stop())
               {
                   ZLYNX_LOG_DEBUG("[{}] thread stopping",zlynx::Thread::get_name());
                   break;
               }
               ++idle_thread_count_;
               idle_fiber->resume();
               --idle_thread_count_;
            }
        }
        ZLYNX_LOG_DEBUG("[{}] thread end",zlynx::Thread::get_name());
    }

    void Scheduler::tickle() const
    {
        uint64_t one = 1;
        ssize_t n = write(tickle_id, &one, sizeof(one));
        if (n != sizeof(one))
        {
            if (errno != EAGAIN)
            {
                ZLYNX_LOG_DEBUG("Scheduler::Tickle() write eventfd failed: {}", strerror(errno));
            }
            else
            {
                ZLYNX_LOG_ERROR("Scheduler::Tickle() write eventfd failed");
            }
        }
    }

    void Scheduler::idle() noexcept
    {
        uint64_t dummy;
        ssize_t n = read(tickle_id, &dummy, sizeof(dummy));
        if (n != sizeof(dummy))
        {
            if (errno != EAGAIN)
            {
                ZLYNX_LOG_DEBUG("Scheduler::idle() read eventfd failed: {}", strerror(errno));
            }
            else
            {
                ZLYNX_LOG_ERROR("Scheduler::idle() read eventfd failed");
            }
        }
    }
}
