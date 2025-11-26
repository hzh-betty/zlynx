#ifndef ZLYNX_SCHEDULER_H
#define ZLYNX_SCHEDULER_H

#include <vector>
#include <memory>
#include <atomic>
#include <deque>

#include "mutex.hpp"
#include "thread.h"
#include "fiber.h"


namespace zlynx
{
    class Scheduler
    {
    public:
        using MutexType = zlynx::Mutex;

        explicit Scheduler(int thread_count = 1, bool use_caller = true,
                           std::string  name = "");

        virtual ~Scheduler();

        const std::string& name()const { return name_; }

        // 启动调度器
        void start();

        // 停止调度器, 并等待所有线程执行完毕
        void stop();

        // 是否停止
        bool is_stop() const;

        // 投递任务
        template <class F, class... Args>
        void schedule(F&& f, Args&&... args) {
            schedule(std::make_shared<Fiber>(std::bind(std::forward<F>(f), std::forward<Args>(args)...)));
        }

        void schedule(Fiber::ptr fiber);

        static Scheduler* get_this();
        static Fiber* get_scheduler_fiber();

    protected:
        // 每个线程主循环
        virtual void run() noexcept;

        // 唤醒至少一个idle线程
        void tickle() const;

        // idle线程执行的协程
        virtual void idle() noexcept;

        bool has_idle_threads() const { return idle_thread_count_ > 0; }
    private:
        struct Task {
            Fiber::ptr fiber; // 任务协程
            std::function<void()> callback; // 任务回调函数

            Task() = default;

            explicit Task(Fiber::ptr f)
                : fiber(std::move(f)) {}

            explicit Task(std::function<void()> cb)
                : callback(std::move(cb)) {}

            void reset() {
                fiber = nullptr;
                callback = nullptr;
            }
        };

        std::string name_; // 调度器名称
        mutable MutexType mutex_; // 互斥锁，保护共享数据
        std::deque<Task> tasks_; // 任务队列

        bool use_caller_ = false; // 是否使用调用线程作为调度协程
        std::vector<Thread::ptr> threads_; // 线程池

        int tickle_id = -1; // 唤醒标识
        std::atomic<int> total_thread_count_{0}; // 线程总数
        std::atomic<int> active_thread_count_{0}; // 活跃线程计数
        std::atomic<int> idle_thread_count_{0}; // 空闲线程计数
        std::atomic<bool> stopping_{false}; // 停止标志

        Fiber::ptr root_fiber_; // 主协程
    };

}// namespace zlynx


#endif //ZLYNX_SCHEDULER_H
