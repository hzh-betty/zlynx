#include "hook.h"

#include <dlfcn.h>
#include <cstdarg>

#include "timer.h"
#include "fiber.h"
#include "io_manager.h"
#include "fd_manager.h"
#include "singleton.hpp"
#include "zlynx_logger.h"

// 需要hook的函数列表
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)

namespace zlynx
{
    static thread_local bool t_hook_enable = false;

    bool is_hook_enable()
    {
        return t_hook_enable;
    }

    void set_hook_enable(const bool flag)
    {
        t_hook_enable = flag;
    }

    void hook_init()
    {
        static bool is_inited = false;
        if (is_inited) return;
        is_inited = true;

        // sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
        HOOK_FUN(XX)
#undef XX
    }

    struct HookIniter
    {
        HookIniter()
        {
            hook_init();
        }
    };

    static HookIniter s_init_hook;
} // namespace zlynx

struct timer_info
{
    int cancelled = 0;
};


template<class OriginFun, class... Args>
static ssize_t do_io_hook(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so,
                          Args &&... args)
{
    // 如果hook未启用，直接调用原始函数
    if (!zlynx::is_hook_enable())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取文件描述符上下文
    zlynx::FdManager &fd_manager = zlynx::Singleton<zlynx::FdManager>::get_instance();
    zlynx::FdCtx::ptr fd_ctx = fd_manager.get_ctx(fd);
    if (!fd_ctx)
    {
        fun(fd, std::forward<Args>(args)...);
    }

    // 如果文件描述符已关闭，设置errno并返回错误
    if (fd_ctx->is_closed())
    {
        errno = EBADF;
        return -1;
    }


    // 如果不是socket或者用户设置为非阻塞，直接调用原始函数
    if (!fd_ctx->is_socket() || !fd_ctx->get_user_nonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    uint64_t timeout = fd_ctx->get_timeout(timeout_so);
    std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();
    ssize_t ret = 0;
    while (true)
    {
        ret = fun(fd, std::forward<Args>(args)...);

        // 如果被系统中断，继续调用
        while (ret == -1 && errno == EINTR)
        {
            ret = fun(fd, std::forward<Args>(args)...);
        }

        // 如果资源不可用，进行协程调度
        if (ret == -1 && errno == EAGAIN)
        {
            zlynx::IoManager *iom = zlynx::IoManager::get_this();
            std::shared_ptr<zlynx::Timer> timer = nullptr;
            std::weak_ptr<timer_info> winfo(tinfo);

            // 如果设置了超时时间，添加定时器
            if (timeout != static_cast<uint64_t>(-1))
            {
                timer = iom->add_condition_timer(timeout, [winfo,fd,iom,event]()
                {
                    const auto it = winfo.lock();
                    if (!it || it->cancelled)
                    {
                        return;
                    }
                    it->cancelled = ETIMEDOUT;

                    // 取消定时器
                    iom->cancel_event(fd, static_cast<zlynx::IoManager::Event>(event));
                }, winfo);
            }
            // 添加事件监听
            int add_event_ret = iom->add_event(fd, static_cast<zlynx::IoManager::Event>(event));
            if (add_event_ret)
            {
                ZLYNX_LOG_WARN("{} add_event failed, fd={}, event={}, ret={}", hook_fun_name, fd, event, add_event_ret);
                if (timer)
                {
                    timer->cancel();
                }
                return -1;
            }
            zlynx::Fiber::ptr cur_fiber = zlynx::Fiber::get_fiber();
            cur_fiber->yield();

            // 协程被唤醒后，检查定时器是否被取消
            if (timer)
            {
                timer->cancel();
            }

            // 如果被定时器取消，设置errno并返回错误
            if (tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }

            // 否则，继续执行原始函数
        }
        else
        {
            break;
        }
    }
    return ret;
}

extern "C" {
#define XX(name) name##_fun name##_f = nullptr;
HOOK_FUN(XX)
#undef XX

unsigned int sleep(unsigned int seconds)
{
    // 如果hook未启用，调用原始sleep函数
    if (!zlynx::is_hook_enable())
    {
        return sleep_f(seconds);
    }

    // 使用协程和定时器实现非阻塞sleep
    zlynx::Fiber::ptr cur_fiber = zlynx::Fiber::get_fiber();
    zlynx::IoManager *iom = zlynx::IoManager::get_this();
    iom->add_timer(seconds * 1000, [iom,cur_fiber]()
    {
        iom->schedule(cur_fiber);
    });
    cur_fiber->yield();
    return 0;
}

int usleep(useconds_t usec)
{
    if (!zlynx::is_hook_enable())
    {
        return usleep_f(usec);
    }

    zlynx::Fiber::ptr cur_fiber = zlynx::Fiber::get_fiber();
    zlynx::IoManager *iom = zlynx::IoManager::get_this();
    iom->add_timer(usec / 1000, [iom,cur_fiber]()
    {
        iom->schedule(cur_fiber);
    });
    cur_fiber->yield();
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!zlynx::is_hook_enable())
    {
        return nanosleep_f(req, rem);
    }

    const long timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    zlynx::Fiber::ptr cur_fiber = zlynx::Fiber::get_fiber();
    zlynx::IoManager *iom = zlynx::IoManager::get_this();
    iom->add_timer(timeout_ms, [iom,cur_fiber]()
    {
        iom->schedule(cur_fiber);
    });

    cur_fiber->yield();
    return 0;
}

int socket(int domain, int type, int protocol)
{
    if (!zlynx::is_hook_enable())
    {
        return socket_f(domain, type, protocol);
    }

    int fd = socket_f(domain, type, protocol);
    if (fd == -1)
    {
        ZLYNX_LOG_ERROR("socket failed");
        return fd;
    }

    zlynx::FdManager &fd_manager = zlynx::Singleton<zlynx::FdManager>::get_instance();
    zlynx::FdCtx::ptr fd_ctx = fd_manager.get_ctx(fd, true);
    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms)
{
    if (!zlynx::is_hook_enable())
    {
        return connect_f(fd, addr, addrlen);
    }

    // 获取文件描述符上下文
    zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd);
    if (!ctx || ctx->is_closed())
    {
        errno = EBADF;
        return -1;
    }

    // 不是socket，调用原始connect函数
    if (!ctx->is_socket())
    {
        return connect_f(fd, addr, addrlen);
    }

    if (!ctx->get_user_nonblock())
    {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if (n == 0) return 0;
    if (n != -1 || errno != EINPROGRESS)
    {
        return n;
    }

    zlynx::IoManager *iom = zlynx::IoManager::get_this();
    std::shared_ptr<zlynx::Timer> timer = nullptr;
    std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();
    std::weak_ptr<timer_info> winfo(tinfo);

    // 如果设置了超时时间，添加定时器
    if (timeout_ms != static_cast<uint64_t>(-1))
    {
        timer = iom->add_condition_timer(timeout_ms, [winfo,fd,iom]()
        {
            const auto it = winfo.lock();
            if (!it || it->cancelled)
            {
                return;
            }
            it->cancelled = ETIMEDOUT;

            // 取消写事件
            iom->cancel_event(fd, zlynx::IoManager::kWrite);
        }, winfo);
    }

    // 添加事件监听
    int add_event_ret = iom->add_event(fd, zlynx::IoManager::kWrite);
    if (add_event_ret)
    {
        if (timer)
        {
            timer->cancel();
        }
    }
    else
    {
        zlynx::Fiber::ptr cur_fiber = zlynx::Fiber::get_fiber();
        cur_fiber->yield();

        // 协程被唤醒后，检查定时器是否被取消
        if (timer)
        {
            timer->cancel();
        }

        // 如果被定时器取消，设置errno并返回错误
        if (tinfo->cancelled)
        {
            errno = tinfo->cancelled;
            return -1;
        }
    }

    int sock_err = 0;
    socklen_t len = sizeof(sock_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == -1)
    {
        return -1;
    }

    if (!sock_err) return 0;

    errno = sock_err;
    return -1;
}

static uint64_t s_connect_timeout = -1;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd = static_cast<int>(do_io_hook(sockfd, accept_f, "accept",
                                         zlynx::IoManager::kRead, SO_RCVTIMEO, addr, addrlen));
    if (fd >= 0)
    {
        zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{
    return do_io_hook(fd, read_f, "read",
                      zlynx::IoManager::kRead, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    return do_io_hook(fd, readv_f, "readv",
                      zlynx::IoManager::kRead, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int fd, void *buf, size_t count, int flags)
{
    return do_io_hook(fd, recv_f, "recv",
                      zlynx::IoManager::kRead, SO_RCVTIMEO, buf, count, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t count, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    return do_io_hook(fd, recvfrom_f, "recvfrom",
                      zlynx::IoManager::kRead, SO_RCVTIMEO, buf, count, flags, src_addr, addrlen);
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    return do_io_hook(fd, recvmsg_f, "recvmsg",
                      zlynx::IoManager::kRead, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return do_io_hook(fd, write_f, "write",
                      zlynx::IoManager::kWrite, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    return do_io_hook(fd, writev_f, "writev",
                      zlynx::IoManager::kWrite, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int fd, const void *buf, size_t count, int flags)
{
    return do_io_hook(fd, send_f, "send",
                      zlynx::IoManager::kWrite, SO_SNDTIMEO, buf, count, flags);
}

ssize_t sendto(int fd, const void *buf, size_t count, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    return do_io_hook(fd, sendto_f, "sendto",
                      zlynx::IoManager::kWrite, SO_SNDTIMEO, buf, count, flags, dest_addr, addrlen);
}

ssize_t sentmsg(int fd, const struct msghdr *msg, int flags)
{
    return do_io_hook(fd, sendmsg_f, "sendmsg",
                      zlynx::IoManager::kWrite, SO_SNDTIMEO, msg, flags);
}

int close(int fd)
{
    if (!zlynx::is_hook_enable())
    {
        return close_f(fd);
    }

    // 获取文件描述符上下文
    zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd);
    if (ctx)
    {
        auto iom = zlynx::IoManager::get_this();
        if (iom)
        {
            iom->cancel_all(fd);
        }

        zlynx::Singleton<zlynx::FdManager>::get_instance().delete_ctx(fd);
    }

    return close_f(fd);
}

int fcntl(int fd, int cmd, ... /* arg */)
{
    va_list va; // to access a list of mutable parameters

    va_start(va, cmd);
    switch (cmd)
    {
        case F_SETFL:
            {
                int arg = va_arg(va, int); // Access the next int argument
                va_end(va);
                zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd);

                // 如果上下文不存在、已关闭或不是socket，调用原始fcntl函数
                if (!ctx || ctx->is_closed() || !ctx->is_socket())
                {
                    return fcntl_f(fd, cmd, arg);
                }

                // 用户是否设定了非阻塞
                ctx->set_user_nonblock(!!(arg & O_NONBLOCK));

                // 最后是否阻塞根据系统设置决定
                if (ctx->get_sys_nonblock())
                {
                    arg |= O_NONBLOCK;
                }
                else
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }

        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd);
                if (!ctx || ctx->is_closed() || !ctx->is_socket())
                {
                    return arg;
                }
                // 这里是呈现给用户 显示的为用户设定的值
                // 但是底层还是根据系统设置决定的
                if (ctx->get_user_nonblock())
                {
                    return arg | O_NONBLOCK;
                }
                else
                {
                    return arg & ~O_NONBLOCK;
                }
            }

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }

        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }

        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock *arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }

        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list va;
    va_start(va, request);
    void *arg = va_arg(va, void*);
    va_end(va);

    // 如果request是FIONBIO，设置用户非阻塞标志
    if (FIONBIO == request)
    {
        bool user_nonblock = !!*static_cast<int *>(arg);
        zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(fd);
        if (!ctx || ctx->is_closed() || !ctx->is_socket())
        {
            return ioctl_f(fd, request, arg);
        }
        ctx->set_user_nonblock(user_nonblock);
    }
    return ioctl_f(fd, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    if (!::zlynx::is_hook_enable())
    {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    if (level == SOL_SOCKET)
    {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
        {
           zlynx::FdCtx::ptr ctx = zlynx::Singleton<zlynx::FdManager>::get_instance().get_ctx(sockfd);
            if (ctx)
            {
                const auto *v = static_cast<const timeval *>(optval);
                ctx->set_timeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}
}
