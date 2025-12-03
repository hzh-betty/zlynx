#include <sys/epoll.h>
#include <unistd.h>

#include <stdexcept>

#include "io_manager.h"
#include "zlynx_logger.h"

namespace zlynx
{
    IoManager::FbContext::EventContext &IoManager::FbContext::get_context(const Event event)
    {
        switch (event)
        {
            case kRead:
                return read;
            case kWrite:
                return write;
            default: ;
        }
        ZLYNX_LOG_ERROR("IoManager::FbContext::get_context invalid event: {}", static_cast<int>(event));
        throw std::runtime_error("IoManager::FbContext::get_context invalid event");
    }

    void IoManager::FbContext::trigger_event(const Event event)
    {
        ZLYNX_LOG_DEBUG("IoManager::FbContext::trigger_event fd={} event={}", fd, static_cast<int>(event));
        events = static_cast<Event>(events & ~event); // 移除已触发的事件
        EventContext &ctx = get_context(event);
        if (ctx.callback)
        {
            ctx.scheduler->schedule(ctx.callback);
        }
        else
        {
            ctx.scheduler->schedule(ctx.fiber);
        }
        ctx.reset();
        ZLYNX_LOG_DEBUG("Io::Manager::FbContext::trigger_event fd={} event={} triggered", fd, static_cast<int>(event));
    }

    IoManager::IoManager(const int thread_count, const bool use_caller, const std::string &name)
        : Scheduler(thread_count, use_caller, name),
          TimerManager()
    {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1)
        {
            ZLYNX_LOG_FATAL("IoManager::IoManager epoll_create1 failed: {}", strerror(errno));
            return;
        }
        context_resize(64);

        // tickle_id
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = tickle_id;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tickle_id, &event) == -1)
        {
            ZLYNX_LOG_FATAL("IoManager::IoManager epoll_ctl add tickle_id failed: {}", strerror(errno));
            close(epoll_fd_);
            epoll_fd_ = -1;
            return;
        }

        start(); // 启动调度器
        ZLYNX_LOG_INFO("IoManager::IoManager started");
    }

    IoManager::~IoManager()
    {
        stop();

        if (epoll_fd_ != -1)
        {
            close(epoll_fd_);
        }

        for (const auto &ctx: fb_contexts_)
        {
            delete ctx;
        }
    }

    int IoManager::add_event(int fd, const Event event, std::function<void()> cb)
    {
        ZLYNX_LOG_DEBUG("IoManager::add_event fd={}", fd);
        FbContext *ctx = nullptr;
        {
            RWMutexType::ReadLock lock(mutex_);
            if (fd >= static_cast<int>(fb_contexts_.size()))
            {
                lock.unlock();
                RWMutexType::WriteLock write_lock(mutex_);
                context_resize(static_cast<int>(fd * 1.5));
            }
            ctx = fb_contexts_[fd];
        }

        FbContext::MutexType::Lock lock(ctx->mutex_);
        if (ctx->events & event)
        {
            ZLYNX_LOG_WARN("IoManager::add_event fd={} event already registered", fd);
            return -1;
        }

        const int op = ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD; // 修改或添加
        epoll_event epevent{};
        epevent.events = EPOLLET | ctx->events | event;
        epevent.data.ptr = ctx;
        const int ret = epoll_ctl(epoll_fd_, op, fd, &epevent);
        if (ret == -1)
        {
            ZLYNX_LOG_ERROR("IoManager::add_event epoll_ctl failed: {}", strerror(errno));
            return -1;
        }
        ++pending_event_count_;

        ctx->events = static_cast<Event>(ctx->events | event);

        // 设置事件上下文
        FbContext::EventContext &event_ctx = ctx->get_context(event);
        event_ctx.scheduler = Scheduler::get_this();
        if (cb)
        {
            event_ctx.callback = std::move(cb);
        }
        else
        {
            event_ctx.fiber = Fiber::get_fiber();
        }
        ZLYNX_LOG_DEBUG("IoManager::add_event fd={} event added", fd);
        return 0;
    }

    IoManager::FbContext *IoManager::del_event_helper(int fd, IoManager::Event event, bool update)
    {
        // 获取上下文
        RWMutexType::ReadLock lock(mutex_);
        if (fd >= static_cast<int>(fb_contexts_.size()))
        {
            ZLYNX_LOG_ERROR("IoManager::del_event_helper fd={} not exist", fd);
            return nullptr;
        }
        FbContext *ctx = fb_contexts_[fd];
        lock.unlock();

        FbContext::MutexType::Lock lock2(ctx->mutex_);
        if (!(ctx->events & event))
        {
            ZLYNX_LOG_WARN("IoManager::del_event_helper fd={} event not registered", fd);
            return nullptr;
        }

        const auto new_events = static_cast<Event>(ctx->events & ~event);;
        const int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 修改或删除
        epoll_event epevent{};
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = ctx;
        const int ret = epoll_ctl(epoll_fd_, op, fd, &epevent);
        if (ret == -1)
        {
            ZLYNX_LOG_ERROR("IoManager::del_event_helper epoll_ctl failed: {}", strerror(errno));
            return nullptr;
        }

        if (update) ctx->events = new_events;
        return ctx;
    }

    bool IoManager::del_event(const int fd, const Event event)
    {
        ZLYNX_LOG_DEBUG("IoManager::del_event fd={}", fd);
        FbContext *ctx = del_event_helper(fd, event, true);
        if (!ctx) return false;
        --pending_event_count_;

        // 重置事件上下文
        FbContext::EventContext &event_ctx = ctx->get_context(event);
        event_ctx.reset();
        ZLYNX_LOG_DEBUG("IoManager::del_event fd={} event deleted", fd);
        return true;
    }

    bool IoManager::cancel_event(const int fd, const Event event)
    {
        ZLYNX_LOG_DEBUG("IoManager::cancel_event fd={}", fd);
        FbContext *ctx = del_event_helper(fd, event, false);
        if (!ctx) return false;
        --pending_event_count_;

        ctx->trigger_event(event);
        ZLYNX_LOG_DEBUG("IoManager::cancel_event fd={}", fd);
        return true;
    }

    bool IoManager::cancel_all(const int fd)
    {
        ZLYNX_LOG_DEBUG("IoManager::cancel_all fd={}", fd);
        FbContext *ctx = del_event_helper(fd, static_cast<Event>(~0), false);
        if (!ctx) return false;

        if (ctx->events & kRead)
        {
            ctx->trigger_event(kRead);
            --pending_event_count_;
        }
        if (ctx->events & kWrite)
        {
            ctx->trigger_event(kWrite);
            --pending_event_count_;
        }
        ZLYNX_LOG_DEBUG("IoManager::cancel_all fd={}", fd);
        return true;
    }

    /*
     * 为了实现边缘触发(epoll ET)模式，所以为了避免一个事件重复触发，
     * 每次触发事件后都需要从epoll中删除该事件，等待下一次添加时再重新注册。
     */
    void IoManager::idle() noexcept
    {
        ZLYNX_LOG_DEBUG("IoManager::idle start");
        constexpr int MAX_EVENTS = 256;
        auto *events = new epoll_event[MAX_EVENTS];
        std::unique_ptr<epoll_event[]> guard(events);

        while (true)
        {
            if (is_stop())
            {
                ZLYNX_LOG_DEBUG("IoManager::idle stopping");
                break;
            }

            // 计算下一个定时器超时时间
            uint64_t next_timeout = get_next_expire_time();
            if (next_timeout == ~0ull)
            {
                next_timeout = 3000; // 默认3秒
            }

            // 等待I/O事件或定时器超时
            const int n = epoll_wait(epoll_fd_, events, MAX_EVENTS,
                                     static_cast<int>(next_timeout));

            if (n == -1 || errno == EINTR) continue;

            // 处理到期的定时器回调
            std::vector<std::function<void()> > callbacks;
            list_expired_callbacks(callbacks);
            for (const auto &callback: callbacks)
            {
                schedule(callback);
            }

            // 处理I/O事件
            for (int i = 0; i < n; ++i)
            {
                epoll_event &event = events[i];
                if (event.data.fd == tickle_id)
                {
                    uint64_t dummy;
                    const ssize_t ret = read(tickle_id, &dummy, sizeof(dummy));
                    (void) ret;
                    continue;
                }

                auto *ctx = static_cast<FbContext *>(events[i].data.ptr);
                FbContext::MutexType::Lock lock(ctx->mutex_);

                // 处理错误事件
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    event.events |= ((EPOLLIN | EPOLLOUT) & ctx->events); // 触发读写事件
                }

                // 确定实际触发的事件
                int real_events = kNone;
                if (event.events & EPOLLIN)
                {
                    real_events |= kRead;
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= kWrite;
                }

                // 如果没有感兴趣的事件，继续下一个
                if ((ctx->events & real_events) == kNone)
                {
                    continue;
                }

                const int left_events = ctx->events & (~real_events); // 剩余未触发的事件
                const int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 修改或删除
                event.events = EPOLLET | left_events;
                const int ret = epoll_ctl(epoll_fd_, op, ctx->fd, &event);
                if (ret == -1)
                {
                    ZLYNX_LOG_WARN("IoManager::idle epoll_ctl failed: {}", strerror(errno));
                    continue;
                }

                // 处理读事件
                if (real_events & kRead)
                {
                    ctx->trigger_event(kRead);
                    --pending_event_count_;
                }

                // 处理写事件
                if (real_events & kWrite)
                {
                    ctx->trigger_event(kWrite);
                    --pending_event_count_;
                }
            }

            Fiber::get_fiber()->yield(); // 让出协程
        }
        ZLYNX_LOG_DEBUG("IoManager::idle end");
    }

    bool IoManager::is_stop()
    {
        return get_next_expire_time() == ~0ull && pending_event_count_ == 0 && Scheduler::is_stop();
    }

    void IoManager::on_timer_inserted_at_front()
    {
        tickle();
    }

    void IoManager::context_resize(const size_t size)
    {
        fb_contexts_.resize(size);
        for (size_t i = 0; i < size; i++)
        {
            fb_contexts_[i] = new FbContext();
            fb_contexts_[i]->fd = static_cast<int>(i);
        }
    }
} // namespace zlynx
