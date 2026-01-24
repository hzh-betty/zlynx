#include "hook.h"
#include "fiber.h"
#include "io_scheduler.h"
#include "status_table.h"
#include "thread_context.h"
#include "zcoroutine_logger.h"
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>

namespace zcoroutine {

/**
 * @brief 检查当前线程是否启用了hook
 */
bool is_hook_enabled() { return ThreadContext::is_hook_enabled(); }

/**
 * @brief 设置当前线程的hook启用状态
 */
void set_hook_enable(bool enable) { ThreadContext::set_hook_enable(enable); }

/**
 * @brief Hook初始化函数
 *
 * 使用dlsym获取libc中的原始系统调用函数地址，保存到全局变量中。
 * 这样在Hook函数中就可以调用原始的系统调用。
 */
void hook_init() {
  static bool is_inited = false;
  if (is_inited)
    return;
  is_inited = true;

  // 使用dlsym获取原始函数指针
  // RTLD_NEXT 表示在动态链接库搜索顺序中，查找下一个出现的符号
#define XX(name) name##_f = (name##_func)dlsym(RTLD_NEXT, #name);
  XX(sleep)
  XX(usleep)
  XX(nanosleep)
  XX(socket)
  XX(socketpair)
  XX(connect)
  XX(accept)
  XX(accept4)
  XX(read)
  XX(readv)
  XX(recv)
  XX(recvfrom)
  XX(recvmsg)
  XX(write)
  XX(writev)
  XX(send)
  XX(sendto)
  XX(sendmsg)
  XX(fcntl)
  XX(ioctl)
  XX(close)
  XX(shutdown)
  XX(setsockopt)
  XX(getsockopt)
#undef XX
}

// 静态初始化器，确保hook在main之前初始化
struct HookIniter {
  HookIniter() { hook_init(); }
};

static HookIniter s_hook_initer;

} // namespace zcoroutine

// 当 hook 代码运行在 scheduler_fiber 上时，绝不能 yield。
// 因为 scheduler_fiber::yield 会切回 main_fiber，导致 worker 的 run()
// 直接退出。
static inline bool in_scheduler_fiber() {
  auto cur = zcoroutine::ThreadContext::get_current_fiber();
  auto sched = zcoroutine::ThreadContext::get_scheduler_fiber();
  return (cur && sched && cur == sched);
}

static inline int do_fcntl(int fd, int cmd, int arg) {
  if (fcntl_f) {
    return fcntl_f(fd, cmd, arg);
  }
  return ::fcntl(fd, cmd, arg);
}

template <typename Fn>
static auto with_temp_blocking_fd(int fd, Fn &&fn) -> decltype(fn()) {
  int flags = do_fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return fn();
  }
  const bool was_nonblock = (flags & O_NONBLOCK) != 0;
  if (was_nonblock) {
    (void)do_fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }

  auto ret = fn();

  if (was_nonblock) {
    (void)do_fcntl(fd, F_SETFL, flags);
  }
  return ret;
}

// 定义原始函数指针
sleep_func sleep_f = nullptr;
usleep_func usleep_f = nullptr;
nanosleep_func nanosleep_f = nullptr;
socket_func socket_f = nullptr;
socketpair_func socketpair_f = nullptr;
connect_func connect_f = nullptr;
accept_func accept_f = nullptr;
accept4_func accept4_f = nullptr;
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
shutdown_func shutdown_f = nullptr;
setsockopt_func setsockopt_f = nullptr;
getsockopt_func getsockopt_f = nullptr;

// 定时器信息，用于超时处理
struct timer_info {
  std::atomic<int> cancelled{0};
};

// 默认连接超时时间（毫秒）
static uint64_t s_connect_timeout = 5000;

/**
 * @brief 通用IO Hook模板函数
 *
 * 这是Hook机制的核心实现。它将同步阻塞的IO操作转换为异步非阻塞+协程调度的模式。
 *
 * 流程如下：
 * 1. 检查Hook是否启用，未启用则直接调用原始函数。
 * 2. 检查fd是否是Socket且非用户设置的非阻塞模式。
 * 3. 尝试执行一次原始IO操作。
 * 4. 如果操作返回EAGAIN（表示资源不可用），则：
 *    a. 向IoScheduler注册IO事件监听。
 *    b. 如果有超时设置，注册一个定时器。
 *    c. 让出当前协程执行权 (Fiber::yield)。
 *    d. 当事件就绪或超时后，协程恢复执行。
 *    e. 再次尝试IO操作。
 *
 * @param fd 文件描述符
 * @param fun 原始函数
 * @param hook_fun_name 函数名（用于日志）
 * @param event IO事件类型（读/写）
 * @param timeout_so 超时选项类型（SO_RCVTIMEO/SO_SNDTIMEO）
 * @param args 原始函数的其他参数
 * @return 返回值与原始函数相同
 */
template <typename OriginFun, typename... Args>
static ssize_t do_io_hook(int fd, OriginFun fun, const char *hook_fun_name,
                          uint32_t event, int timeout_so, Args &&...args) {
  // 如果hook未启用，直接调用原始函数
  if (!zcoroutine::is_hook_enabled()) {
    return fun(fd, std::forward<Args>(args)...);
  }

  // 获取文件描述符上下文
  auto *status_table = zcoroutine::StatusTable::GetInstance();
  zcoroutine::SocketStatus::ptr fd_ctx = status_table->get(fd);
  if (!fd_ctx) {
    return fun(fd, std::forward<Args>(args)...);
  }

  // 如果文件描述符已关闭，设置errno并返回错误
  if (fd_ctx->is_closed()) {
    errno = EBADF;
    return -1;
  }

  // 如果不是socket或者用户设置为非阻塞，直接调用原始函数
  // 注意：我们只hook阻塞模式的socket IO
  if (!fd_ctx->is_socket() || fd_ctx->get_user_nonblock()) {
    return fun(fd, std::forward<Args>(args)...);
  }

  // scheduler_fiber 上禁止 yield：退化为“临时改回阻塞”直接调用原始 syscall。
  // 说明：这会阻塞调度线程，但能保证 callback 直接执行时不因 yield 导致 worker
  // 退出。
  if (in_scheduler_fiber()) {
    (void)hook_fun_name;
    (void)event;
    (void)timeout_so;
    return with_temp_blocking_fd(fd, [&]() {
      ssize_t ret = fun(fd, std::forward<Args>(args)...);
      while (ret == -1 && errno == EINTR) {
        ret = fun(fd, std::forward<Args>(args)...);
      }
      return ret;
    });
  }

  // 获取超时时间
  uint64_t timeout = fd_ctx->get_timeout(timeout_so);
  std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();

  while (true) {
    // 尝试执行IO操作
    ssize_t ret = fun(fd, std::forward<Args>(args)...);

    // 如果被系统中断，重试
    while (ret == -1 && errno == EINTR) {
      ret = fun(fd, std::forward<Args>(args)...);
    }

    // 如果资源暂时不可用（EAGAIN），进行协程调度
    if (ret == -1 && errno == EAGAIN) {
      zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
      if (!iom) {
        return fun(fd, std::forward<Args>(args)...);
      }

      zcoroutine::Timer::ptr timer = nullptr;
      std::weak_ptr<timer_info> winfo(tinfo);

      // 如果设置了超时时间，添加条件定时器
      if (timeout != static_cast<uint64_t>(-1) && timeout != 0) {
        timer = iom->add_condition_timer(
            timeout,
            [winfo, fd, iom, event]() {
              auto it = winfo.lock();
              if (!it || it->cancelled) {
                return;
              }
              it->cancelled = ETIMEDOUT;
              // 取消IO事件，这会触发epoll事件从而唤醒协程
              iom->cancel_event(
                  fd, static_cast<zcoroutine::Channel::Event>(event));
            },
            winfo);
      }

      // 添加IO事件监听
      int add_event_ret =
          iom->add_event(fd, static_cast<zcoroutine::Channel::Event>(event));
      if (add_event_ret != 0) {
        ZCOROUTINE_LOG_WARN("{} add_event failed, fd={}, event={}, ret={}",
                            hook_fun_name, fd, event, add_event_ret);
        if (timer) {
          timer->cancel();
        }
        return -1;
      }

      // 让出协程，等待事件就绪或超时
      zcoroutine::Fiber::yield();

      // 协程被唤醒后，取消定时器
      if (timer) {
        timer->cancel();
      }

      // 检查是否超时
      if (tinfo->cancelled) {
        errno = tinfo->cancelled;
        return -1;
      }

      // 重新尝试IO操作
      continue;
    }

    return ret;
  }
}

extern "C" {

// Sleep系列函数的Hook实现原理：
// 添加一个定时器，然后让出协程。当定时器超时后，调度器会重新调度该协程。

unsigned int sleep(unsigned int seconds) {
  if (!zcoroutine::is_hook_enabled()) {
    return sleep_f(seconds);
  }

  // scheduler_fiber 上禁止 yield：直接走原始 sleep
  if (in_scheduler_fiber()) {
    return sleep_f(seconds);
  }

  zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
  if (!iom) {
    return sleep_f(seconds);
  }

  zcoroutine::Fiber::ptr cur_fiber = zcoroutine::Fiber::get_this();
  if (!cur_fiber) {
    return sleep_f(seconds);
  }

  // 使用定时器和协程实现非阻塞sleep
  // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
  iom->add_timer(seconds * 1000,
                 [iom, cur_fiber]() { iom->schedule(cur_fiber); });
  zcoroutine::Fiber::yield();

  return 0;
}

int usleep(useconds_t usec) {
  if (!zcoroutine::is_hook_enabled()) {
    return usleep_f(usec);
  }

  // scheduler_fiber 上禁止 yield：直接走原始 usleep
  if (in_scheduler_fiber()) {
    return usleep_f(usec);
  }

  zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
  if (!iom) {
    return usleep_f(usec);
  }

  zcoroutine::Fiber::ptr cur_fiber = zcoroutine::Fiber::get_this();
  if (!cur_fiber) {
    return usleep_f(usec);
  }

  // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
  iom->add_timer(usec / 1000, [iom, cur_fiber]() { iom->schedule(cur_fiber); });
  zcoroutine::Fiber::yield();

  return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!zcoroutine::is_hook_enabled()) {
    return nanosleep_f(req, rem);
  }

  // scheduler_fiber 上禁止 yield：直接走原始 nanosleep
  if (in_scheduler_fiber()) {
    return nanosleep_f(req, rem);
  }

  zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
  if (!iom) {
    return nanosleep_f(req, rem);
  }

  ZCOROUTINE_LOG_DEBUG("hook nanosleep");

  zcoroutine::Fiber::ptr cur_fiber = zcoroutine::Fiber::get_this();
  if (!cur_fiber) {
    return nanosleep_f(req, rem);
  }

  uint64_t timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
  // 必须在yield之前捕获shared_ptr，保持协程存活直到定时器触发
  iom->add_timer(timeout_ms, [iom, cur_fiber]() { iom->schedule(cur_fiber); });
  zcoroutine::Fiber::yield();

  return 0;
}

// ==================== Socket系列 ====================

int socket(int domain, int type, int protocol) {
  if (!zcoroutine::is_hook_enabled()) {
    return socket_f(domain, type, protocol);
  }

  int fd = socket_f(domain, type, protocol);
  if (fd < 0) {
    return fd;
  }

  // 获取 StatusTable 并注册该fd
  // 这一步很重要，因为我们需要追踪每个socket的状态（是否是非阻塞，超时设置等）
  zcoroutine::StatusTable::GetInstance()->get(fd, true); // auto_create = true

  ZCOROUTINE_LOG_DEBUG("hook::socket fd={}", fd);
  return fd;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  if (!zcoroutine::is_hook_enabled()) {
    return socketpair_f(domain, type, protocol, sv);
  }

  int ret = socketpair_f(domain, type, protocol, sv);
  if (ret < 0) {
    return ret;
  }

  // 获取 StatusTable 并注册两个fd
  zcoroutine::StatusTable::GetInstance()->get(sv[0],
                                              true); // auto_create = true
  zcoroutine::StatusTable::GetInstance()->get(sv[1],
                                              true); // auto_create = true

  ZCOROUTINE_LOG_DEBUG("hook::socketpair sv[0]={}, sv[1]={}", sv[0], sv[1]);
  return ret;
}

/**
 * @brief 带超时的connect实现
 *
 * connect的Hook比较特殊，因为connect不能像read/write那样简单重试。
 * 非阻塞connect的标准处理流程是：
 * 1. 发起connect，如果返回EINPROGRESS，表示正在连接。
 * 2. 使用epoll/select等待socket可写。
 * 3. 可写后，使用getsockopt检查SO_ERROR判断连接是否成功。
 */
int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen,
                         uint64_t timeout_ms) {
  if (!zcoroutine::is_hook_enabled()) {
    return connect_f(fd, addr, addrlen);
  }

  // 获取文件描述符上下文
  zcoroutine::SocketStatus::ptr ctx =
      zcoroutine::StatusTable::GetInstance()->get(fd);
  if (!ctx || ctx->is_closed()) {
    errno = EBADF;
    return -1;
  }

  // 不是socket，调用原始connect函数
  if (!ctx->is_socket()) {
    return connect_f(fd, addr, addrlen);
  }

  // 用户设置为非阻塞，直接调用
  if (ctx->get_user_nonblock()) {
    return connect_f(fd, addr, addrlen);
  }

  // scheduler_fiber 上禁止 yield：退化为阻塞 connect。
  // 尽力支持超时：通过 SO_SNDTIMEO 影响阻塞 connect
  // 的等待时间（不同内核/协议栈可能不完全等价）。
  if (in_scheduler_fiber()) {
    timeval old_tv{};
    socklen_t old_len = sizeof(old_tv);
    bool has_old = false;
    if (getsockopt_f &&
        getsockopt_f(fd, SOL_SOCKET, SO_SNDTIMEO, &old_tv, &old_len) == 0) {
      has_old = true;
    }

    if (timeout_ms != static_cast<uint64_t>(-1)) {
      timeval tv{};
      tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
      tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
      if (setsockopt_f) {
        (void)setsockopt_f(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      } else {
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      }
    }

    int ret = with_temp_blocking_fd(fd, [&]() {
      int n = connect_f(fd, addr, addrlen);
      while (n == -1 && errno == EINTR) {
        n = connect_f(fd, addr, addrlen);
      }
      return n;
    });

    if (has_old && setsockopt_f) {
      (void)setsockopt_f(fd, SOL_SOCKET, SO_SNDTIMEO, &old_tv, old_len);
    } else if (has_old) {
      (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &old_tv, old_len);
    }

    return ret;
  }

  // 尝试连接
  int n = connect_f(fd, addr, addrlen);
  if (n == 0) {
    return 0;
  }
  // EINPROGRESS表示连接正在进行中
  if (n != -1 || errno != EINPROGRESS) {
    return n;
  }

  // 连接正在进行中，等待可写事件
  zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
  if (!iom) {
    return n;
  }

  zcoroutine::Timer::ptr timer = nullptr;
  std::shared_ptr<timer_info> tinfo = std::make_shared<timer_info>();
  std::weak_ptr<timer_info> winfo(tinfo);

  // 如果设置了超时时间，添加定时器
  if (timeout_ms != static_cast<uint64_t>(-1) && timeout_ms != 0) {
    timer = iom->add_condition_timer(
        timeout_ms,
        [winfo, fd, iom]() {
          auto it = winfo.lock();
          if (!it || it->cancelled) {
            return;
          }
          it->cancelled = ETIMEDOUT;
          iom->cancel_event(fd, zcoroutine::Channel::kWrite);
        },
        winfo);
  }
  // 添加写事件监听
  int add_event_ret = iom->add_event(fd, zcoroutine::Channel::kWrite);
  if (add_event_ret != 0) {
    if (timer) {
      timer->cancel();
    }
    ZCOROUTINE_LOG_ERROR("connect_with_timeout add_event failed, fd={}", fd);
    return -1;
  }

  // 让出协程，等待连接完成或超时
  zcoroutine::Fiber::yield();

  // 取消定时器
  if (timer) {
    timer->cancel();
  }

  // 检查是否超时
  if (tinfo->cancelled) {
    errno = tinfo->cancelled;
    return -1;
  }

  // 检查连接结果
  int sock_err = 0;
  socklen_t len = sizeof(sock_err);
  if (getsockopt_f(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == -1) {
    return -1;
  }

  if (sock_err != 0) {
    errno = sock_err;
    return -1;
  }

  return 0;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  int fd = static_cast<int>(do_io_hook(sockfd, accept_f, "accept",
                                       zcoroutine::Channel::kRead,
                                       SO_RCVTIMEO, addr, addrlen));
  if (fd >= 0) {
    // 注册新连接的fd
    zcoroutine::StatusTable::GetInstance()->get(fd, true);
  }
  return fd;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
  // 如果 accept4 符号不可用，则退化为 accept（flags 语义由上层负责兜底）
  if (!accept4_f) {
    int fd = static_cast<int>(do_io_hook(sockfd, accept_f, "accept4->accept",
                                         zcoroutine::Channel::kRead,
                                         SO_RCVTIMEO, addr, addrlen));
    if (fd >= 0) {
      zcoroutine::StatusTable::GetInstance()->get(fd, true);
    }
    return fd;
  }

  int fd = static_cast<int>(do_io_hook(sockfd, accept4_f, "accept4",
                                       zcoroutine::Channel::kRead,
                                       SO_RCVTIMEO, addr, addrlen, flags));
  if (fd >= 0) {
    zcoroutine::StatusTable::GetInstance()->get(fd, true);
  }
  return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
  return do_io_hook(fd, read_f, "read", zcoroutine::Channel::kRead,
                    SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  return do_io_hook(fd, readv_f, "readv", zcoroutine::Channel::kRead,
                    SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
  return do_io_hook(sockfd, recv_f, "recv", zcoroutine::Channel::kRead,
                    SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
  return do_io_hook(sockfd, recvfrom_f, "recvfrom",
                    zcoroutine::Channel::kRead, SO_RCVTIMEO, buf, len, flags,
                    src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
  return do_io_hook(sockfd, recvmsg_f, "recvmsg", zcoroutine::Channel::kRead,
                    SO_RCVTIMEO, msg, flags);
}

// ==================== Write系列 ====================

ssize_t write(int fd, const void *buf, size_t count) {
  return do_io_hook(fd, write_f, "write", zcoroutine::Channel::kWrite,
                    SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  return do_io_hook(fd, writev_f, "writev", zcoroutine::Channel::kWrite,
                    SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
  return do_io_hook(sockfd, send_f, "send", zcoroutine::Channel::kWrite,
                    SO_SNDTIMEO, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
  return do_io_hook(sockfd, sendto_f, "sendto", zcoroutine::Channel::kWrite,
                    SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
  return do_io_hook(sockfd, sendmsg_f, "sendmsg", zcoroutine::Channel::kWrite,
                    SO_SNDTIMEO, msg, flags);
}

// ==================== close ====================

int close(int fd) {
  if (!zcoroutine::is_hook_enabled()) {
    return close_f(fd);
  }

  ZCOROUTINE_LOG_DEBUG("hook close fd={}", fd);

  // 获取文件描述符上下文
  zcoroutine::SocketStatus::ptr ctx =
      zcoroutine::StatusTable::GetInstance()->get(fd);
  if (ctx) {
    zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
    if (iom) {
      // 取消该fd上的所有事件
      iom->cancel_all(fd);
    }
    // 从 StatusTable 中删除
    zcoroutine::StatusTable::GetInstance()->del(fd);
  }

  return close_f(fd);
}

// ==================== shutdown ====================

int shutdown(int sockfd, int how) {
  if (!zcoroutine::is_hook_enabled()) {
    return shutdown_f(sockfd, how);
  }

  // 获取文件描述符上下文
  zcoroutine::SocketStatus::ptr ctx =
      zcoroutine::StatusTable::GetInstance()->get(sockfd);
  if (!ctx || ctx->is_closed() || !ctx->is_socket()) {
    return shutdown_f(sockfd, how);
  }

  int ret = shutdown_f(sockfd, how);
  while (ret == -1 && errno == EINTR) {
    ret = shutdown_f(sockfd, how);
  }

  // shutdown 成功后，尽量唤醒/取消对应方向的等待事件，避免协程长期挂起
  if (ret == 0) {
    zcoroutine::IoScheduler *iom = zcoroutine::IoScheduler::get_this();
    if (iom) {
      switch (how) {
      case SHUT_RD:
        (void)iom->cancel_event(sockfd, zcoroutine::Channel::kRead);
        break;
      case SHUT_WR:
        (void)iom->cancel_event(sockfd, zcoroutine::Channel::kWrite);
        break;
      case SHUT_RDWR:
        (void)iom->cancel_all(sockfd);
        break;
      default:
        break;
      }
    }
  }

  return ret;
}

// ==================== fcntl ====================

/**
 * @brief fcntl hook
 *
 * 这里的关键逻辑是维护用户视角和系统视角的非阻塞状态。
 * 在协程框架中，socket在系统层面始终是 O_NONBLOCK 的。
 * 但为了兼容用户代码，我们需要记录用户是否设置了 O_NONBLOCK。
 *
 * 当用户设置 O_NONBLOCK 时，我们记录下来，并且系统socket保持 O_NONBLOCK。
 * 当用户清除 O_NONBLOCK 时，我们记录下来，但系统socket仍然保持 O_NONBLOCK。
 * 当用户查询 flags 时，我们根据记录的用户状态，伪造返回结果。
 */
int fcntl(int fd, int cmd, ... /* arg */) {
  va_list va;
  va_start(va, cmd);

  switch (cmd) {
  // 带int参数的命令
  case F_SETFL: {
    int arg = va_arg(va, int);
    va_end(va);

    zcoroutine::SocketStatus::ptr ctx =
        zcoroutine::StatusTable::GetInstance()->get(fd);
    if (!ctx || ctx->is_closed() || !ctx->is_socket()) {
      return fcntl_f(fd, cmd, arg);
    }

    // 记录用户设置的非阻塞状态
    ctx->set_user_nonblock(arg & O_NONBLOCK);

    // 系统层面始终保持非阻塞
    if (ctx->get_sys_nonblock()) {
      arg |= O_NONBLOCK;
    } else {
      arg &= ~O_NONBLOCK;
    }
    return fcntl_f(fd, cmd, arg);
  }

  case F_GETFL: {
    va_end(va);
    int ret = fcntl_f(fd, cmd);

    zcoroutine::SocketStatus::ptr ctx =
        zcoroutine::StatusTable::GetInstance()->get(fd);
    if (!ctx || ctx->is_closed() || !ctx->is_socket()) {
      return ret;
    }

    // 返回用户视角的非阻塞状态
    if (ctx->get_user_nonblock()) {
      return ret | O_NONBLOCK;
    } else {
      return ret & ~O_NONBLOCK;
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
  case F_GETLK: {
    struct flock *arg = va_arg(va, struct flock *);
    va_end(va);
    return fcntl_f(fd, cmd, arg);
  }

  case F_GETOWN_EX:
  case F_SETOWN_EX: {
    struct f_owner_ex *arg = va_arg(va, struct f_owner_ex *);
    va_end(va);
    return fcntl_f(fd, cmd, arg);
  }

  default: {
    va_end(va);
    return fcntl_f(fd, cmd);
  }
  }
}

// ==================== ioctl ====================

int ioctl(int fd, unsigned long request, ...) {
  va_list va;
  va_start(va, request);
  void *arg = va_arg(va, void *);
  va_end(va);

  // 处理非阻塞设置
  if (request == FIONBIO) {
    bool user_nonblock = !!*static_cast<int *>(arg);

    zcoroutine::SocketStatus::ptr ctx =
        zcoroutine::StatusTable::GetInstance()->get(fd);
    if (ctx && !ctx->is_closed() && ctx->is_socket()) {
      ctx->set_user_nonblock(user_nonblock);
    }
  }

  return ioctl_f(fd, request, arg);
}

// ==================== setsockopt/getsockopt ====================

int setsockopt(int sockfd, int level, int optname, const void *optval,
               socklen_t optlen) {
  if (!zcoroutine::is_hook_enabled()) {
    return setsockopt_f(sockfd, level, optname, optval, optlen);
  }

  // 处理超时设置
  if (level == SOL_SOCKET) {
    if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
      zcoroutine::SocketStatus::ptr ctx =
          zcoroutine::StatusTable::GetInstance()->get(sockfd);
      if (ctx) {
        const auto *tv = static_cast<const timeval *>(optval);
        uint64_t timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
        ctx->set_timeout(optname, timeout_ms);
      }
    }
  }

  return setsockopt_f(sockfd, level, optname, optval, optlen);
}

int getsockopt(int sockfd, int level, int optname, void *optval,
               socklen_t *optlen) {
  return getsockopt_f(sockfd, level, optname, optval, optlen);
}

} // extern "C"
