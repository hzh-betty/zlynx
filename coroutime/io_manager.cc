#include "io_manager.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>

#include "zlynx_logger.h"

namespace zlynx
{
    // FdContext implementation
    FdContext::EventContext &FdContext::get_context(IoEvent event)
    {
        switch (event)
        {
            case IoEvent::READ:
                return read;
            case IoEvent::WRITE:
                return write;
            case IoEvent::NONE:
                ZLYNX_LOG_ERROR("FdContext::get_context NONE is not a valid event type");
                throw std::invalid_argument("NONE is not a valid event type");
            default:
                ZLYNX_LOG_ERROR("FdContext::get_context invalid event type");
                throw std::invalid_argument("invalid event type");
        }
    }

    void FdContext::reset_context(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.callback = nullptr;
    }

    void FdContext::trigger_event(IoEvent event)
    {
        uint32_t event_val = static_cast<uint32_t>(event);
        if (!(events & event_val))
        {
            ZLYNX_LOG_WARN("FdContext::trigger_event event not registered");
            return;
        }

        // 清除事件标志
        events = events & ~event_val;

        EventContext &ctx = get_context(event);
        if (ctx.callback)
        {
            ctx.scheduler->schedule(std::move(ctx.callback));
        }
        else if (ctx.fiber)
        {
            ctx.scheduler->schedule(std::move(ctx.fiber));
        }
        reset_context(ctx);
    }

    // IoManager implementation
    IoManager::IoManager(int threads, bool use_caller, const std::string &name)
        : Scheduler(threads, use_caller, name)
    {
        // 创建epoll实例
        epfd_ = epoll_create(5000);
        if (epfd_ < 0)
        {
            ZLYNX_LOG_FATAL("IoManager::IoManager epoll_create failed");
            throw std::runtime_error("epoll_create failed");
        }

        // 创建tickle管道
        if (pipe(tickle_fds_) < 0)
        {
            close(epfd_);
            ZLYNX_LOG_FATAL("IoManager::IoManager pipe failed");
            throw std::runtime_error("pipe failed");
        }

        // 设置管道读端为非阻塞
        if (fcntl(tickle_fds_[0], F_SETFL, O_NONBLOCK) < 0)
        {
            close(epfd_);
            close(tickle_fds_[0]);
            close(tickle_fds_[1]);
            ZLYNX_LOG_FATAL("IoManager::IoManager fcntl failed");
            throw std::runtime_error("fcntl failed");
        }

        // 注册管道读端到epoll
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = tickle_fds_[0];
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, tickle_fds_[0], &event) < 0)
        {
            close(epfd_);
            close(tickle_fds_[0]);
            close(tickle_fds_[1]);
            ZLYNX_LOG_FATAL("IoManager::IoManager epoll_ctl failed");
            throw std::runtime_error("epoll_ctl failed");
        }

        resize_fd_contexts(32);

        ZLYNX_LOG_INFO("IoManager::IoManager started");

        // 启动调度器
        start();
    }

    IoManager::~IoManager()
    {
        stop();

        close(epfd_);
        close(tickle_fds_[0]);
        close(tickle_fds_[1]);

        for (auto *ctx : fd_contexts_)
        {
            if (ctx)
            {
                delete ctx;
            }
        }
    }

    void IoManager::resize_fd_contexts(size_t size)
    {
        fd_contexts_.resize(size);
        for (size_t i = 0; i < fd_contexts_.size(); ++i)
        {
            if (!fd_contexts_[i])
            {
                fd_contexts_[i] = new FdContext();
                fd_contexts_[i]->fd = static_cast<int>(i);
            }
        }
    }

    bool IoManager::add_event(int fd, IoEvent event, std::function<void()> cb)
    {
        FdContext *fd_ctx = nullptr;

        {
            Mutex::Lock lock(mutex_);
            if (static_cast<int>(fd_contexts_.size()) <= fd)
            {
                // Use safe growth strategy with bounds checking
                size_t new_size = static_cast<size_t>(fd) + 1;
                if (new_size < static_cast<size_t>(fd) * 3 / 2)
                {
                    new_size = static_cast<size_t>(fd) * 3 / 2;
                }
                resize_fd_contexts(new_size);
            }
            fd_ctx = fd_contexts_[fd];
        }

        FdContext::MutexType::Lock lock(fd_ctx->mutex);

        // 检查事件是否已注册
        uint32_t event_val = static_cast<uint32_t>(event);
        if (fd_ctx->events & event_val)
        {
            ZLYNX_LOG_ERROR("IoManager::add_event event already exists, fd={}, event={}", fd, event_val);
            return false;
        }

        // 确定epoll操作类型
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event ep_event{};
        ep_event.events = EPOLLET | fd_ctx->events | event_val;
        ep_event.data.ptr = fd_ctx;

        if (epoll_ctl(epfd_, op, fd, &ep_event) < 0)
        {
            ZLYNX_LOG_ERROR("IoManager::add_event epoll_ctl failed, fd={}, op={}, error={}", fd, op, strerror(errno));
            return false;
        }

        // 更新事件标志
        fd_ctx->events = fd_ctx->events | event_val;
        ++pending_event_count_;

        // 设置回调
        FdContext::EventContext &event_ctx = fd_ctx->get_context(event);
        if (event_ctx.scheduler || event_ctx.fiber || event_ctx.callback)
        {
            ZLYNX_LOG_WARN("IoManager::add_event event context not empty");
        }

        event_ctx.scheduler = Scheduler::get_this();
        if (cb)
        {
            event_ctx.callback = std::move(cb);
        }
        else
        {
            event_ctx.fiber = Fiber::get_fiber();
        }

        return true;
    }

    bool IoManager::del_event(int fd, IoEvent event)
    {
        FdContext *fd_ctx = nullptr;

        {
            Mutex::Lock lock(mutex_);
            if (static_cast<int>(fd_contexts_.size()) <= fd)
            {
                return false;
            }
            fd_ctx = fd_contexts_[fd];
        }

        FdContext::MutexType::Lock lock(fd_ctx->mutex);

        uint32_t event_val = static_cast<uint32_t>(event);
        if (!(fd_ctx->events & event_val))
        {
            return false;
        }

        uint32_t new_events = fd_ctx->events & ~event_val;
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event ep_event{};
        ep_event.events = EPOLLET | new_events;
        ep_event.data.ptr = fd_ctx;

        if (epoll_ctl(epfd_, op, fd, &ep_event) < 0)
        {
            ZLYNX_LOG_ERROR("IoManager::del_event epoll_ctl failed, fd={}, op={}", fd, op);
            return false;
        }

        --pending_event_count_;
        fd_ctx->events = new_events;
        FdContext::EventContext &event_ctx = fd_ctx->get_context(event);
        fd_ctx->reset_context(event_ctx);

        return true;
    }

    bool IoManager::cancel_event(int fd, IoEvent event)
    {
        FdContext *fd_ctx = nullptr;

        {
            Mutex::Lock lock(mutex_);
            if (static_cast<int>(fd_contexts_.size()) <= fd)
            {
                return false;
            }
            fd_ctx = fd_contexts_[fd];
        }

        FdContext::MutexType::Lock lock(fd_ctx->mutex);

        uint32_t event_val = static_cast<uint32_t>(event);
        if (!(fd_ctx->events & event_val))
        {
            return false;
        }

        uint32_t new_events = fd_ctx->events & ~event_val;
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event ep_event{};
        ep_event.events = EPOLLET | new_events;
        ep_event.data.ptr = fd_ctx;

        if (epoll_ctl(epfd_, op, fd, &ep_event) < 0)
        {
            ZLYNX_LOG_ERROR("IoManager::cancel_event epoll_ctl failed, fd={}, op={}", fd, op);
            return false;
        }

        // 触发事件回调
        fd_ctx->trigger_event(event);
        --pending_event_count_;

        return true;
    }

    bool IoManager::cancel_all(int fd)
    {
        FdContext *fd_ctx = nullptr;

        {
            Mutex::Lock lock(mutex_);
            if (static_cast<int>(fd_contexts_.size()) <= fd)
            {
                return false;
            }
            fd_ctx = fd_contexts_[fd];
        }

        FdContext::MutexType::Lock lock(fd_ctx->mutex);

        if (!fd_ctx->events)
        {
            return false;
        }

        int op = EPOLL_CTL_DEL;
        epoll_event ep_event{};
        ep_event.events = 0;
        ep_event.data.ptr = fd_ctx;

        if (epoll_ctl(epfd_, op, fd, &ep_event) < 0)
        {
            ZLYNX_LOG_ERROR("IoManager::cancel_all epoll_ctl failed, fd={}", fd);
            return false;
        }

        // 触发所有事件回调
        if (fd_ctx->events & static_cast<uint32_t>(IoEvent::READ))
        {
            fd_ctx->trigger_event(IoEvent::READ);
            --pending_event_count_;
        }
        if (fd_ctx->events & static_cast<uint32_t>(IoEvent::WRITE))
        {
            fd_ctx->trigger_event(IoEvent::WRITE);
            --pending_event_count_;
        }

        return true;
    }

    IoManager *IoManager::get_this()
    {
        return dynamic_cast<IoManager *>(Scheduler::get_this());
    }

    void IoManager::tickle()
    {
        if (!has_idle_threads())
        {
            return;
        }

        if (write(tickle_fds_[1], "T", 1) < 0)
        {
            ZLYNX_LOG_ERROR("IoManager::tickle write failed");
        }
    }

    bool IoManager::is_stop() const
    {
        return Scheduler::is_stop() && pending_event_count_ == 0 && !has_timer();
    }

    void IoManager::idle() noexcept
    {
        ZLYNX_LOG_DEBUG("IoManager::idle started");

        const int MAX_EVENTS = 256;
        auto *events = new epoll_event[MAX_EVENTS];

        while (true)
        {
            if (is_stop())
            {
                ZLYNX_LOG_DEBUG("IoManager::idle stopping");
                break;
            }

            int ret = 0;
            do
            {
                static const int MAX_TIMEOUT = 5000;  // 5秒超时
                uint64_t next_timeout = get_next_expire_time();
                int timeout = static_cast<int>(std::min(next_timeout, static_cast<uint64_t>(MAX_TIMEOUT)));

                ret = epoll_wait(epfd_, events, MAX_EVENTS, timeout);
                if (ret < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    ZLYNX_LOG_ERROR("IoManager::idle epoll_wait failed: {}", strerror(errno));
                    break;
                }
                break;
            } while (true);

            // 处理到期的定时器
            std::vector<Timer::Callback> cbs;
            list_expired_callbacks(cbs);
            if (!cbs.empty())
            {
                for (auto &cb : cbs)
                {
                    schedule(cb);
                }
                cbs.clear();
            }

            // 处理IO事件
            for (int i = 0; i < ret; ++i)
            {
                epoll_event &event = events[i];

                // 处理tickle事件
                if (event.data.fd == tickle_fds_[0])
                {
                    char dummy[256];
                    while (read(tickle_fds_[0], dummy, sizeof(dummy)) > 0)
                    {
                        // 清空管道
                    }
                    continue;
                }

                auto *fd_ctx = static_cast<FdContext *>(event.data.ptr);
                FdContext::MutexType::Lock lock(fd_ctx->mutex);

                // 处理错误和挂起
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }

                uint32_t real_events = 0;
                if (event.events & EPOLLIN)
                {
                    real_events |= static_cast<uint32_t>(IoEvent::READ);
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= static_cast<uint32_t>(IoEvent::WRITE);
                }

                if ((fd_ctx->events & real_events) == 0)
                {
                    continue;
                }

                // 剩余事件
                uint32_t left_events = fd_ctx->events & ~real_events;
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = EPOLLET | left_events;

                if (epoll_ctl(epfd_, op, fd_ctx->fd, &event) < 0)
                {
                    ZLYNX_LOG_ERROR("IoManager::idle epoll_ctl failed");
                    continue;
                }

                // 触发事件回调
                if (real_events & static_cast<uint32_t>(IoEvent::READ))
                {
                    fd_ctx->trigger_event(IoEvent::READ);
                    --pending_event_count_;
                }
                if (real_events & static_cast<uint32_t>(IoEvent::WRITE))
                {
                    fd_ctx->trigger_event(IoEvent::WRITE);
                    --pending_event_count_;
                }
            }

            // 让出协程
            Fiber::get_fiber()->yield();
        }

        delete[] events;
        ZLYNX_LOG_DEBUG("IoManager::idle ended");
    }

    void IoManager::on_timer_inserted_at_front()
    {
        tickle();
    }

} // namespace zlynx
