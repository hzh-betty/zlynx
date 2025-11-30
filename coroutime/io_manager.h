#ifndef ZLYNX_IO_MANAGER_H
#define ZLYNX_IO_MANAGER_H

#include <sys/epoll.h>
#include <memory>
#include <functional>
#include <vector>

#include "scheduler.h"
#include "timer.h"
#include "mutex.hpp"

namespace zlynx
{
    /**
     * @brief IO事件类型
     */
    enum class IoEvent : uint32_t
    {
        NONE = 0,
        READ = EPOLLIN,
        WRITE = EPOLLOUT
    };

    /**
     * @brief IO事件上下文，用于存储fd相关的事件回调
     */
    struct FdContext
    {
        using MutexType = Mutex;

        /**
         * @brief 事件上下文，包含事件回调和调度器
         */
        struct EventContext
        {
            Scheduler *scheduler = nullptr;  // 事件执行的调度器
            Fiber::ptr fiber;                // 事件协程
            std::function<void()> callback;  // 事件回调函数
        };

        /**
         * @brief 获取指定事件类型的事件上下文
         * @param event 事件类型
         * @return 事件上下文引用
         */
        EventContext &get_context(IoEvent event);

        /**
         * @brief 重置指定事件类型的事件上下文
         * @param ctx 事件上下文
         */
        void reset_context(EventContext &ctx);

        /**
         * @brief 触发指定事件类型的回调
         * @param event 事件类型
         */
        void trigger_event(IoEvent event);

        int fd = 0;                   // 文件描述符
        EventContext read;            // 读事件上下文
        EventContext write;           // 写事件上下文
        uint32_t events = 0;          // 当前注册的事件类型
        MutexType mutex;              // 互斥锁
    };

    /**
     * @brief IO管理器，继承自Scheduler和TimerManager
     * 使用epoll实现IO多路复用，支持定时器功能
     */
    class IoManager : public Scheduler, public TimerManager
    {
    public:
        using ptr = std::shared_ptr<IoManager>;

        /**
         * @brief 构造函数
         * @param threads 线程数量
         * @param use_caller 是否使用调用线程
         * @param name 调度器名称
         */
        explicit IoManager(int threads = 1, bool use_caller = true,
                          const std::string &name = "");

        /**
         * @brief 析构函数
         */
        ~IoManager() override;

        /**
         * @brief 添加IO事件
         * @param fd 文件描述符
         * @param event 事件类型
         * @param cb 回调函数
         * @return 是否添加成功
         */
        bool add_event(int fd, IoEvent event, std::function<void()> cb = nullptr);

        /**
         * @brief 删除IO事件
         * @param fd 文件描述符
         * @param event 事件类型
         * @return 是否删除成功
         */
        bool del_event(int fd, IoEvent event);

        /**
         * @brief 取消IO事件
         * @param fd 文件描述符
         * @param event 事件类型
         * @return 是否取消成功
         */
        bool cancel_event(int fd, IoEvent event);

        /**
         * @brief 取消所有IO事件
         * @param fd 文件描述符
         * @return 是否取消成功
         */
        bool cancel_all(int fd);

        /**
         * @brief 获取当前IoManager实例
         * @return IoManager指针
         */
        static IoManager *get_this();

    protected:
        /**
         * @brief 通知调度器有新任务到达
         */
        void tickle() override;

        /**
         * @brief 判断是否可以停止
         * @return 是否可以停止
         */
        bool is_stop() const override;

        /**
         * @brief 空闲协程执行的函数
         */
        void idle() noexcept override;

        /**
         * @brief 定时器插入到最前面时的回调
         */
        void on_timer_inserted_at_front() override;

        /**
         * @brief 调整fd上下文数组大小
         * @param size 新的大小
         */
        void resize_fd_contexts(size_t size);

    private:
        int epfd_ = 0;                              // epoll文件描述符
        int tickle_fds_[2] = {0, 0};               // 用于tickle的管道
        std::atomic<size_t> pending_event_count_{0}; // 等待执行的事件数量
        mutable Mutex mutex_;                      // 互斥锁
        std::vector<FdContext *> fd_contexts_;     // fd上下文数组
    };

} // namespace zlynx

#endif // ZLYNX_IO_MANAGER_H
