#ifndef ZCOROUTINE_IO_SCHEDULER_H
#define ZCOROUTINE_IO_SCHEDULER_H

#include <memory>
#include <atomic>
#include "scheduling/scheduler.h"
#include "io/epoll_poller.h"
#include "io/fd_manager.h"
#include "timer/timer_manager.h"
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief IO调度器
 * 
 * 组合Scheduler、EpollPoller和TimerManager，提供IO事件调度功能。
 * 使用组合而非继承的设计模式，职责更加清晰。
 */
class IoScheduler : public NonCopyable {
public:
    using ptr = std::shared_ptr<IoScheduler>;
    
    /**
     * @brief 创建IoScheduler实例
     * @param thread_count 线程数量
     * @param use_caller 是否使用调用线程
     * @param name 调度器名称
     */
    static IoScheduler::ptr CreateInstance(int thread_count = 1, bool use_caller = true, 
                                          const std::string& name = "IoScheduler");
    
    /**
     * @brief 构造函数
     * @param thread_count 线程数量
     * @param use_caller 是否使用调用线程
     * @param name 调度器名称
     */
    explicit IoScheduler(int thread_count = 1, bool use_caller = true, 
                        const std::string& name = "IoScheduler");
    
    /**
     * @brief 析构函数
     */
    ~IoScheduler();
    
    /**
     * @brief 启动IO调度器
     */
    void start();
    
    /**
     * @brief 停止IO调度器
     */
    void stop();
    
    /**
     * @brief 调度协程
     * @param fiber 协程指针
     */
    void schedule(Fiber::ptr fiber);
    
    /**
     * @brief 调度函数
     * @param func 函数对象
     */
    void schedule(std::function<void()> func);
    
    /**
     * @brief 添加IO事件
     * @param fd 文件描述符
     * @param event 事件类型（FdContext::kRead或kWrite）
     * @param callback 事件回调函数
     * @return 成功返回0，失败返回-1
     */
    int add_event(int fd, FdContext::Event event, std::function<void()> callback = nullptr);
    
    /**
     * @brief 删除IO事件
     * @param fd 文件描述符
     * @param event 事件类型
     * @return 成功返回0，失败返回-1
     */
    int del_event(int fd, FdContext::Event event);
    
    /**
     * @brief 取消IO事件
     * @param fd 文件描述符
     * @param event 事件类型
     * @return 成功返回0，失败返回-1
     */
    int cancel_event(int fd, FdContext::Event event);
    
    /**
     * @brief 添加定时器
     * @param timeout 超时时间（毫秒）
     * @param callback 定时器回调
     * @param recurring 是否循环
     * @return 定时器指针
     */
    Timer::ptr add_timer(uint64_t timeout, std::function<void()> callback, bool recurring = false);
    
    /**
     * @brief 获取Scheduler
     */
    Scheduler::ptr scheduler() const { return scheduler_; }
    
    /**
     * @brief 获取TimerManager
     */
    TimerManager::ptr timer_manager() const { return timer_manager_; }
    
    /**
     * @brief 获取单例
     */
    static IoScheduler::ptr GetInstance();

private:
    /**
     * @brief IO线程运行函数
     */
    void io_thread_func();
    
    /**
     * @brief 唤醒IO线程
     */
    void wake_up();

private:
    Scheduler::ptr scheduler_;              // 任务调度器
    EpollPoller::ptr epoll_poller_;         // Epoll封装
    TimerManager::ptr timer_manager_;       // 定时器管理器
    FdManager::ptr fd_manager_;             // 文件描述符管理器
    
    std::unique_ptr<std::thread> io_thread_;  // IO线程
    std::atomic<bool> stopping_;              // 停止标志
    
    int wake_fd_[2];                          // 用于唤醒epoll的管道
    
    static IoScheduler::ptr s_instance_;      // 单例
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_IO_SCHEDULER_H
