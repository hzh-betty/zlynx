#include "io/io_scheduler.h"
#include "zcoroutine_logger.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace zcoroutine {

IoScheduler::IoScheduler(int thread_count, const std::string& name)
    : stopping_(false) {
    
    ZCOROUTINE_LOG_INFO("IoScheduler::IoScheduler initializing name={}, thread_count={}", name, thread_count);
    
    // 创建调度器
    scheduler_ = std::make_shared<Scheduler>(thread_count, name);
    
    // 创建Epoll
    epoll_poller_ = std::make_shared<EpollPoller>(256);
    
    // 创建定时器管理器
    timer_manager_ = std::make_shared<TimerManager>();
    
    // 获取文件描述符管理器
    fd_manager_ = FdManager::GetInstance();
    
    // 创建唤醒管道
    int ret = pipe(wake_fd_);
    if (ret < 0) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::IoScheduler pipe creation failed, errno={}, error={}", 
                             errno, strerror(errno));
        return;
    }
    
    // 设置非阻塞
    fcntl(wake_fd_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_fd_[1], F_SETFL, O_NONBLOCK);
    
    // 将唤醒管道添加到epoll
    epoll_poller_->add_event(wake_fd_[0], EPOLLIN, nullptr);
    
    ZCOROUTINE_LOG_INFO("IoScheduler::IoScheduler initialized successfully, name={}, thread_count={}, wake_fd=[{}, {}]", 
                        name, thread_count, wake_fd_[0], wake_fd_[1]);
}

IoScheduler::~IoScheduler() {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler destroying");
    stop();
    
    if (wake_fd_[0] >= 0) {
        close(wake_fd_[0]);
        ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler closed wake_fd[0]={}", wake_fd_[0]);
    }
    if (wake_fd_[1] >= 0) {
        close(wake_fd_[1]);
        ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler closed wake_fd[1]={}", wake_fd_[1]);
    }
    
    ZCOROUTINE_LOG_INFO("IoScheduler::~IoScheduler destroyed");
}

void IoScheduler::start() {
    ZCOROUTINE_LOG_INFO("IoScheduler::start starting scheduler");
    
    // 启动调度器
    scheduler_->start();
    
    // 启动IO线程
    io_thread_ = std::make_unique<std::thread>([this]() {
        this->io_thread_func();
    });
    
    ZCOROUTINE_LOG_INFO("IoScheduler::start scheduler and IO thread started");
}

void IoScheduler::stop() {
    if (stopping_.load(std::memory_order_relaxed)) {
        ZCOROUTINE_LOG_DEBUG("IoScheduler::stop already stopping, skip");
        return;
    }
    
    stopping_.store(true, std::memory_order_relaxed);
    ZCOROUTINE_LOG_INFO("IoScheduler::stop stopping...");
    
    // 唤醒IO线程
    wake_up();
    
    // 等待IO线程退出
    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
        ZCOROUTINE_LOG_DEBUG("IoScheduler::stop IO thread joined");
    }
    
    // 停止调度器
    if (scheduler_) {
        scheduler_->stop();
    }
    
    ZCOROUTINE_LOG_INFO("IoScheduler::stop stopped successfully");
}

void IoScheduler::schedule(Fiber::ptr fiber) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::schedule fiber name={}, id={}", 
                         fiber->name(), fiber->id());
    scheduler_->schedule(fiber);
    wake_up();  // 唤醒IO线程
}

void IoScheduler::schedule(std::function<void()> func) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::schedule callback function");
    scheduler_->schedule(func);
    wake_up();  // 唤醒IO线程
}

int IoScheduler::add_event(int fd, FdContext::Event event, std::function<void()> callback) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::add_event fd={}, event={}, has_callback={}", 
                         fd, event, callback != nullptr);
    
    // 获取文件描述符上下文
    FdContext::ptr fd_ctx = fd_manager_->get(fd, true);
    if (!fd_ctx) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::add_event failed to get FdContext, fd={}", fd);
        return -1;
    }
    
    // 设置回调或协程
    FdContext::EventContext& event_ctx = fd_ctx->get_event_context(event);
    if (callback) {
        event_ctx.callback = callback;
    } else {
        // 如果没有回调，使用当前协程
        event_ctx.fiber = Fiber::get_this();
    }
    
    // 添加事件到FdContext
    int new_events = fd_ctx->add_event(event);
    
    // 更新epoll
    int op = (fd_ctx->events() == event) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    int ret = 0;
    if (op == EPOLL_CTL_ADD) {
        ret = epoll_poller_->add_event(fd, new_events, fd_ctx.get());
    } else {
        ret = epoll_poller_->mod_event(fd, new_events, fd_ctx.get());
    }
    
    if (ret < 0) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::add_event epoll operation failed, fd={}, event={}, op={}", 
                             fd, event, op == EPOLL_CTL_ADD ? "ADD" : "MOD");
        fd_ctx->del_event(event);
        return -1;
    }
    
    ZCOROUTINE_LOG_DEBUG("IoScheduler::add_event success, fd={}, event={}, new_events={}", 
                         fd, event, new_events);
    return 0;
}

int IoScheduler::del_event(int fd, FdContext::Event event) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::del_event fd={}, event={}", fd, event);
    
    FdContext::ptr fd_ctx = fd_manager_->get(fd, false);
    if (!fd_ctx) {
        ZCOROUTINE_LOG_DEBUG("IoScheduler::del_event FdContext not found, fd={}", fd);
        return 0;
    }
    
    // 删除事件
    int new_events = fd_ctx->del_event(event);
    
    // 更新epoll
    int ret = 0;
    if (new_events == FdContext::kNone) {
        ret = epoll_poller_->del_event(fd);
    } else {
        ret = epoll_poller_->mod_event(fd, new_events, fd_ctx.get());
    }
    
    if (ret < 0) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::del_event epoll operation failed, fd={}, errno={}", 
                             fd, errno);
        return -1;
    }
    
    ZCOROUTINE_LOG_DEBUG("IoScheduler::del_event success, fd={}, event={}, remaining_events={}", 
                         fd, event, new_events);
    return 0;
}

int IoScheduler::cancel_event(int fd, FdContext::Event event) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_event fd={}, event={}", fd, event);
    
    FdContext::ptr fd_ctx = fd_manager_->get(fd, false);
    if (!fd_ctx) {
        ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_event FdContext not found, fd={}", fd);
        return 0;
    }
    
    // 取消事件
    int new_events = fd_ctx->cancel_event(event);
    
    // 更新epoll
    int ret = 0;
    if (new_events == FdContext::kNone) {
        ret = epoll_poller_->del_event(fd);
    } else {
        ret = epoll_poller_->mod_event(fd, new_events, fd_ctx.get());
    }
    
    if (ret < 0) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::cancel_event epoll operation failed, fd={}, errno={}", 
                             fd, errno);
        return -1;
    }
    
    ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_event success, fd={}, event={}, remaining_events={}", 
                         fd, event, new_events);
    return 0;
}

Timer::ptr IoScheduler::add_timer(uint64_t timeout, std::function<void()> callback, bool recurring) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::add_timer timeout={}ms, recurring={}", timeout, recurring);
    auto timer = timer_manager_->add_timer(timeout, callback, recurring);
    wake_up();  // 唤醒IO线程更新超时时间
    return timer;
}

void IoScheduler::io_thread_func() {
    ZCOROUTINE_LOG_INFO("IoScheduler::io_thread_func IO thread started");
    
    std::vector<epoll_event> events;
    
    while (!stopping_.load(std::memory_order_relaxed)) {
        // 获取下一个定时器超时时间
        int timeout = timer_manager_->get_next_timeout();
        if (timeout < 0) {
            timeout = 5000;  // 默认5秒
        }
        
        // 等待IO事件
        int nfds = epoll_poller_->wait(timeout, events);
        
        if (nfds < 0) {
            ZCOROUTINE_LOG_ERROR("IoScheduler::io_thread_func epoll_wait failed, errno={}, error={}", 
                                 errno, strerror(errno));
            continue;
        }
        
        if (nfds > 0) {
            ZCOROUTINE_LOG_DEBUG("IoScheduler::io_thread_func epoll_wait returned nfds={}", nfds);
        }
        
        // 处理就绪事件
        for (int i = 0; i < nfds; ++i) {
            epoll_event& ev = events[i];
            
            // 检查是否是唤醒事件
            if (ev.data.ptr == nullptr) {
                char dummy[256];
                while (read(wake_fd_[0], dummy, sizeof(dummy)) > 0);
                ZCOROUTINE_LOG_DEBUG("IoScheduler::io_thread_func wake up event received");
                continue;
            }
            
            // 处理IO事件
            FdContext* fd_ctx = static_cast<FdContext*>(ev.data.ptr);
            int fd = fd_ctx->fd();
            
            if (ev.events & EPOLLIN) {
                ZCOROUTINE_LOG_DEBUG("IoScheduler::io_thread_func triggering READ event, fd={}", fd);
                fd_ctx->trigger_event(FdContext::kRead);
            }
            if (ev.events & EPOLLOUT) {
                ZCOROUTINE_LOG_DEBUG("IoScheduler::io_thread_func triggering WRITE event, fd={}", fd);
                fd_ctx->trigger_event(FdContext::kWrite);
            }
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                ZCOROUTINE_LOG_WARN("IoScheduler::io_thread_func error/hup event, fd={}, events={}", 
                                    fd, ev.events);
                // 错误事件，同时触发读写
                fd_ctx->trigger_event(FdContext::kRead);
                fd_ctx->trigger_event(FdContext::kWrite);
            }
        }
        
        // 处理超时定时器
        auto expired_cbs = timer_manager_->list_expired_callbacks();
        if (!expired_cbs.empty()) {
            ZCOROUTINE_LOG_DEBUG("IoScheduler::io_thread_func processing {} expired timers", 
                                 expired_cbs.size());
        }
        for (auto& cb : expired_cbs) {
            schedule(cb);
        }
    }
    
    ZCOROUTINE_LOG_INFO("IoScheduler::io_thread_func IO thread exiting");
}

void IoScheduler::wake_up() {
    char dummy = 'W';
    ssize_t n = write(wake_fd_[1], &dummy, 1);
    if (n != 1) {
        ZCOROUTINE_LOG_ERROR("IoScheduler::wake_up write failed, errno={}, error={}", 
                             errno, strerror(errno));
    }
}

IoScheduler::ptr IoScheduler::GetInstance() {
    static IoScheduler::ptr instance = std::make_shared<IoScheduler>(4, "GlobalIoScheduler");
    return instance;
}

}  // namespace zcoroutine
