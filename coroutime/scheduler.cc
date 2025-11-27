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

    Scheduler::Scheduler(int thread_count, const bool use_caller, std::string name)
        : name_(std::move(name)), use_caller_(use_caller)
    {
        if (thread_count <= 0) thread_count = 1;
        if (use_caller_)
        {
            thread_count -= 1;
            Fiber::get_fiber();
            t_scheduler = this;
            root_fiber_ = std::make_shared<Fiber>([this]() { run(); });
            t_scheduler_fiber = root_fiber_.get();
        }
        total_thread_count_ = thread_count;
        tickle_id = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE );
        if (tickle_id == -1)
        {
            throw std::runtime_error("eventfd failed");
        }
    }

    Scheduler::~Scheduler()
    {
        stop();

        if (get_this() == this)
        {
            t_scheduler = nullptr;
        }

        if (tickle_id != -1)
        {
            close(tickle_id);
        }
    }

    void Scheduler::start()
    {
        MutexType::Lock lock(mutex_);

        threads_.reserve(total_thread_count_);
        for (size_t i = 0; i < total_thread_count_; ++i)
        {
            // 创建线程并绑定到调度器的run方法
            auto ptr = std::make_unique<Thread>([this]()
            {
                t_scheduler = this;
                const auto fiber = Fiber::get_fiber();
                t_scheduler_fiber = fiber.get();
                this->run();
            }, name_ + "_" + std::to_string(i));
            threads_.emplace_back(std::move(ptr));
        }
    }

    void Scheduler::stop()
    {
        stopping_ = true;

        for (size_t i = 0; i < threads_.size(); ++i)
        {
            tickle();
        }

        if (use_caller_)
        {
            tickle();
            root_fiber_->resume();
        }

        for (const auto &ptr: threads_)
        {
            ptr->join();
        }
        threads_.clear();
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

    void Scheduler::schedule(Fiber::ptr fiber)
    {
        bool need_tickle = false;
        {
            Mutex::Lock lock(mutex_);
            tasks_.emplace_back(std::move(fiber));
            need_tickle = has_idle_threads();
        }
        if (need_tickle)
        {
            tickle();
        }
    }

    void Scheduler::run() noexcept
    {
        ZLYNX_LOG_DEBUG("[{}] thread start", zlynx::Thread::get_name());
        const auto idle_fiber = std::make_shared<Fiber>([this]()
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
                need_tickle = (!tasks_.empty()) && has_idle_threads();
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
                    --active_thread_count_;
                    if (cb_fiber->state() != Fiber::State::kTerminated)
                    {
                        schedule(std::move(cb_fiber));
                        cb_fiber = nullptr;
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
                    ZLYNX_LOG_DEBUG("[{}] thread stopping", zlynx::Thread::get_name());
                    break;
                }
                ++idle_thread_count_;
                idle_fiber->resume();
                --idle_thread_count_;
            }
        }
        ZLYNX_LOG_DEBUG("[{}] thread end", zlynx::Thread::get_name());
    }

    void Scheduler::tickle() const
    {
        constexpr uint64_t one = 1;
        const ssize_t n = write(tickle_id, &one, sizeof(one));
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
        while (true)
        {
            if (stopping_) break;

            uint64_t dummy;
            const ssize_t n = read(tickle_id, &dummy, sizeof(dummy));

            if (n == sizeof(dummy))
            {
                break;
            }

            if (n < 0 && errno != EAGAIN)
            {
                ZLYNX_LOG_ERROR("Scheduler::idle() read eventfd failed: {}", strerror(errno));
            }

            sleep(1); // 避免空转

        }
    }
}
