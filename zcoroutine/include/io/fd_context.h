#ifndef ZCOROUTINE_FD_CONTEXT_H
#define ZCOROUTINE_FD_CONTEXT_H

#include <functional>
#include <memory>
#include <sys/epoll.h>
#include "runtime/fiber.h"
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief 文件描述符上下文类
 * 
 * 管理单个文件描述符的IO事件和对应的协程回调。
 * 支持读、写两种事件类型，每种事件可以关联一个协程。
 */
class FdContext : public NonCopyable {
public:
    using ptr = std::shared_ptr<FdContext>;
    
    /**
     * @brief 事件类型定义
     */
    enum Event {
        kNone  = 0x0,       // 无事件
        kRead  = EPOLLIN,   // 读事件
        kWrite = EPOLLOUT   // 写事件
    };
    
    /**
     * @brief 事件上下文结构
     * 
     * 存储每个事件类型的协程和回调函数。
     */
    struct EventContext {
        Fiber::ptr fiber;                    // 等待该事件的协程
        std::function<void()> callback;       // 事件回调函数
    };
    
    explicit FdContext(int fd);
    ~FdContext() = default;
    
    /**
     * @brief 获取文件描述符
     */
    int fd() const { return fd_; }
    
    /**
     * @brief 获取当前事件
     */
    int events() const { return events_; }
    
    /**
     * @brief 添加事件
     * @param event 事件类型（kRead或kWrite）
     * @return 添加后的事件
     */
    int add_event(Event event);
    
    /**
     * @brief 删除事件
     * @param event 事件类型
     * @return 删除后的事件
     */
    int del_event(Event event);
    
    /**
     * @brief 取消事件
     * @param event 事件类型
     * @return 取消后的事件
     */
    int cancel_event(Event event);
    
    /**
     * @brief 取消所有事件
     */
    void cancel_all();
    
    /**
     * @brief 触发事件
     * @param event 事件类型
     */
    void trigger_event(Event event);
    
    /**
     * @brief 获取事件上下文
     * @param event 事件类型
     * @return 事件上下文引用
     */
    EventContext& get_event_context(Event event);
    
    /**
     * @brief 重置事件上下文
     * @param ctx 事件上下文
     */
    void reset_event_context(EventContext& ctx);

private:
    int fd_;                    // 文件描述符
    int events_ = kNone;        // 当前注册的事件
    EventContext read_ctx_;     // 读事件上下文
    EventContext write_ctx_;    // 写事件上下文
    std::mutex mutex_;          // 保护上下文的互斥锁
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_FD_CONTEXT_H
