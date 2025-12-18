#include "hook/hook.h"
#include "zcoroutine_logger.h"
#include "io/io_scheduler.h"
#include "runtime/fiber.h"
#include <dlfcn.h>
#include <fcntl.h>

namespace zcoroutine {

// 线程本地的Hook启用标志
static thread_local bool t_hook_enable = false;

bool is_hook_enabled() {
    return t_hook_enable;
}

void set_hook_enable(bool enable) {
    t_hook_enable = enable;
}

}  // namespace zcoroutine

// 定义原始函数指针
sleep_func sleep_f = nullptr;
usleep_func usleep_f = nullptr;
nanosleep_func nanosleep_f = nullptr;
socket_func socket_f = nullptr;
connect_func connect_f = nullptr;
accept_func accept_f = nullptr;
read_func read_f = nullptr;
readv_func readv_f = nullptr;
recv_func recv_f = nullptr;
recvfrom_func recvfrom_f = nullptr;
recvmsg_func recvmsg_f = nullptr;
write_func write_f = nullptr;
writev_func writev_f = nullptr;
send_func send_f = nullptr;
sendto_func sendto_f = nullptr;
sendmsg_func sendmsg_f = nullptr;
fcntl_func fcntl_f = nullptr;
ioctl_func ioctl_f = nullptr;
close_func close_f = nullptr;
setsockopt_func setsockopt_f = nullptr;
getsockopt_func getsockopt_f = nullptr;

// 宏定义：加载原始函数
#define HOOK_FUN(XX) \
    XX ## _f = (XX ## _func)dlsym(RTLD_NEXT, #XX);

// 初始化Hook
struct HookIniter {
    HookIniter() {
        HOOK_FUN(sleep);
        HOOK_FUN(usleep);
        HOOK_FUN(nanosleep);
        HOOK_FUN(socket);
        HOOK_FUN(connect);
        HOOK_FUN(accept);
        HOOK_FUN(read);
        HOOK_FUN(readv);
        HOOK_FUN(recv);
        HOOK_FUN(recvfrom);
        HOOK_FUN(recvmsg);
        HOOK_FUN(write);
        HOOK_FUN(writev);
        HOOK_FUN(send);
        HOOK_FUN(sendto);
        HOOK_FUN(sendmsg);
        HOOK_FUN(fcntl);
        HOOK_FUN(ioctl);
        HOOK_FUN(close);
        HOOK_FUN(setsockopt);
        HOOK_FUN(getsockopt);
        
        ZCOROUTINE_LOG_DEBUG("Hook initialized");
    }
};

static HookIniter s_hook_initer;

extern "C" {

// sleep - 转换为定时器
unsigned int sleep(unsigned int seconds) {
    if (!zcoroutine::is_hook_enabled()) {
        return sleep_f(seconds);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return sleep_f(seconds);
    }
    
    // 添加定时器，超时后继续执行
    io_scheduler->add_timer(seconds * 1000, [](){});
    zcoroutine::Fiber::yield();
    
    return 0;
}

// usleep - 转换为定时器
int usleep(useconds_t usec) {
    if (!zcoroutine::is_hook_enabled()) {
        return usleep_f(usec);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return usleep_f(usec);
    }
    
    // 添加定时器
    io_scheduler->add_timer(usec / 1000, [](){});
    zcoroutine::Fiber::yield();
    
    return 0;
}

// socket - 设置为非阻塞
int socket(int domain, int type, int protocol) {
    if (!zcoroutine::is_hook_enabled()) {
        return socket_f(domain, type, protocol);
    }
    
    int fd = socket_f(domain, type, protocol);
    if (fd < 0) {
        return fd;
    }
    
    // 设置为非阻塞
    int flags = fcntl_f(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    ZCOROUTINE_LOG_DEBUG("hook::socket fd={}", fd);
    return fd;
}

// read - 异步读
ssize_t read(int fd, void *buf, size_t count) {
    if (!zcoroutine::is_hook_enabled()) {
        return read_f(fd, buf, count);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return read_f(fd, buf, count);
    }
    
    // 尝试读取
    ssize_t n = read_f(fd, buf, count);
    if (n >= 0 || errno != EAGAIN) {
        return n;
    }
    
    // 添加读事件，等待可读
    io_scheduler->add_event(fd, zcoroutine::FdContext::kRead);
    zcoroutine::Fiber::yield();
    
    // 重新读取
    return read_f(fd, buf, count);
}

// write - 异步写
ssize_t write(int fd, const void *buf, size_t count) {
    if (!zcoroutine::is_hook_enabled()) {
        return write_f(fd, buf, count);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return write_f(fd, buf, count);
    }
    
    // 尝试写入
    ssize_t n = write_f(fd, buf, count);
    if (n >= 0 || errno != EAGAIN) {
        return n;
    }
    
    // 添加写事件，等待可写
    io_scheduler->add_event(fd, zcoroutine::FdContext::kWrite);
    zcoroutine::Fiber::yield();
    
    // 重新写入
    return write_f(fd, buf, count);
}

// accept - 异步接受连接
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!zcoroutine::is_hook_enabled()) {
        return accept_f(sockfd, addr, addrlen);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return accept_f(sockfd, addr, addrlen);
    }
    
    // 尝试接受连接
    int fd = accept_f(sockfd, addr, addrlen);
    if (fd >= 0 || errno != EAGAIN) {
        // 设置新连接为非阻塞
        if (fd >= 0) {
            int flags = fcntl_f(fd, F_GETFL, 0);
            if (flags != -1) {
                fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
        return fd;
    }
    
    // 添加读事件，等待新连接
    io_scheduler->add_event(sockfd, zcoroutine::FdContext::kRead);
    zcoroutine::Fiber::yield();
    
    // 重新接受连接
    fd = accept_f(sockfd, addr, addrlen);
    if (fd >= 0) {
        int flags = fcntl_f(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl_f(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    return fd;
}

// connect - 异步连接
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!zcoroutine::is_hook_enabled()) {
        return connect_f(sockfd, addr, addrlen);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (!io_scheduler) {
        return connect_f(sockfd, addr, addrlen);
    }
    
    // 尝试连接
    int ret = connect_f(sockfd, addr, addrlen);
    if (ret == 0 || errno != EINPROGRESS) {
        return ret;
    }
    
    // 添加写事件，等待连接完成
    io_scheduler->add_event(sockfd, zcoroutine::FdContext::kWrite);
    zcoroutine::Fiber::yield();
    
    // 检查连接结果
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt_f(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    
    if (error != 0) {
        errno = error;
        return -1;
    }
    
    return 0;
}

// close - 删除事件
int close(int fd) {
    if (!zcoroutine::is_hook_enabled()) {
        return close_f(fd);
    }
    
    auto io_scheduler = zcoroutine::IoScheduler::GetInstance();
    if (io_scheduler) {
        io_scheduler->del_event(fd, zcoroutine::FdContext::kRead);
        io_scheduler->del_event(fd, zcoroutine::FdContext::kWrite);
    }
    
    return close_f(fd);
}

// 其他函数直接调用原始函数（简化实现）
int nanosleep(const struct timespec *req, struct timespec *rem) {
    return nanosleep_f(req, rem);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return readv_f(fd, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return recv_f(sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return recvfrom_f(sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return recvmsg_f(sockfd, msg, flags);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return writev_f(fd, iov, iovcnt);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return send_f(sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return sendto_f(sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    return sendmsg_f(sockfd, msg, flags);
}

int fcntl(int fd, int cmd, ...) {
    va_list va;
    va_start(va, cmd);
    int arg = va_arg(va, int);
    va_end(va);
    return fcntl_f(fd, cmd, arg);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);
    return ioctl_f(fd, request, arg);
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

}  // extern "C"
