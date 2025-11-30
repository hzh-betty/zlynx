#ifndef ZLYNX_IO_MANAGER_H
#define ZLYNX_IO_MANAGER_H


#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>

#include "fiber.h"
#include "rw_mutex.hpp"
#include "scheduler.h"
#include "timer.h"

namespace zlynx
{
    class IoManager final : public Scheduler, public TimerManager
    {
    public:
        using ptr = std::shared_ptr<IoManager>;
        using RWMutexType = RWMutex;

        enum Event
        {
            kNone = 0x0,
            kRead = 0x1, // EPOLLIN
            kWrite = 0x4 // EPOLLOUT
        };

        struct FbContext
        {
            using MutexType = Mutex;

            struct EventContext
            {
                Scheduler *scheduler{nullptr}; // 事件触发时所属的调度器
                Fiber::ptr fiber{nullptr}; // 事件触发时执行的协程
                std::function<void()> callback; // 事件触发时的回调函数

                void reset()
                {
                    scheduler = nullptr;
                    fiber.reset();
                    callback = nullptr;
                }
            };

            // 获取指定事件的上下文
            EventContext& get_context(Event event);
            // 触发指定事件
            void trigger_event(Event event);

            EventContext read; // 读事件上下文
            EventContext write; // 写事件上下文
            int fd{-1}; // 文件描述符
            Event events{kNone}; // 已注册的事件
            MutexType mutex_;
        };

        explicit IoManager(int thread_count = 1, bool use_caller = true,
                           const std::string &name = "IoManager");

        ~IoManager() override;

        // 添加事件监听
        int add_event(int fd, Event event, std::function<void()> cb = nullptr);

        FbContext* del_event_helper(int fd, Event event, bool update = false);

        // 删除事件监听
        bool del_event(int fd, Event event);

        // 取消事件监听,并且触发相应事件
        bool cancel_event(int fd, Event event);

        // 取消所有事件监听，并且触发相应事件
        bool cancel_all(int fd);

        static IoManager *get_this()
        {
            return dynamic_cast<IoManager *>(Scheduler::get_this());
        }
    protected:
        void idle() noexcept override;
        bool is_stop() override;
        void on_timer_inserted_at_front() override;

        void context_resize(size_t size);

    protected:
        int epoll_fd_ = -1; // epoll文件描述符
        std::atomic<size_t> pending_event_count_ = 0; // 待处理事件计数
        std::vector<FbContext*> fb_contexts_; // 文件描述符上下文集合
        RWMutexType mutex_;
    };
}
#endif //ZLYNX_IO_MANAGER_H
