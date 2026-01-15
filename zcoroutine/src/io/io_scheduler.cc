#include "io/io_scheduler.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "io/fd_context_table.h"
#include "scheduling/scheduler.h"
#include "util/zcoroutine_logger.h"
namespace zcoroutine {
FdContext::ptr IoScheduler::get_fd_context(int fd, bool auto_create) {
  if (auto_create) {
    return fd_context_table_->get_or_create(fd);
  }
  return fd_context_table_->get(fd);
}

IoScheduler::IoScheduler(int thread_count, const std::string &name,
                         bool use_shared_stack)
    : Scheduler(thread_count, name, use_shared_stack) {
  ZCOROUTINE_LOG_INFO(
      "IoScheduler::IoScheduler initializing name={}, thread_count={}", name,
      thread_count);

  // 创建 Epoll
  constexpr size_t kDefaultMaxEvents = 256;
  epoll_poller_ = std::make_shared<EpollPoller>(kDefaultMaxEvents);

  // 创建定时器管理器
  timer_manager_ = std::make_shared<TimerManager>();

  // 设置定时器插入回调：当有新定时器插入到最前面时，唤醒IO线程
  timer_manager_->set_on_timer_inserted_at_front([this]() { this->wake_up(); });

  // 创建 FdContext 表
  fd_context_table_ = std::make_unique<FdContextTable>();

  // 创建唤醒管道
  int ret = pipe(wake_fd_);
  if (ret < 0) {
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::IoScheduler pipe creation failed, errno={}, error={}",
        errno, strerror(errno));
    return;
  }

  // 设置非阻塞
  fcntl(wake_fd_[0], F_SETFL, O_NONBLOCK);
  fcntl(wake_fd_[1], F_SETFL, O_NONBLOCK);

  // 将唤醒管道添加到epoll
  epoll_poller_->add_event(wake_fd_[0], EPOLLIN, nullptr);

  ZCOROUTINE_LOG_INFO("IoScheduler::IoScheduler initialized successfully, "
                      "name={}, thread_count={}, wake_fd=[{}, {}]",
                      name, thread_count, wake_fd_[0], wake_fd_[1]);
}

IoScheduler::~IoScheduler() {
  ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler destroying");
  stop();

  if (wake_fd_[0] >= 0) {
    close(wake_fd_[0]);
    ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler closed wake_fd[0]={}",
                         wake_fd_[0]);
  }
  if (wake_fd_[1] >= 0) {
    close(wake_fd_[1]);
    ZCOROUTINE_LOG_DEBUG("IoScheduler::~IoScheduler closed wake_fd[1]={}",
                         wake_fd_[1]);
  }

  ZCOROUTINE_LOG_INFO("IoScheduler::~IoScheduler destroyed");
}

void IoScheduler::start() {
  ZCOROUTINE_LOG_INFO("IoScheduler::start starting scheduler");

  // 启动基类调度器
  Scheduler::start();

  // 启动IO线程
  io_thread_ =
      std::make_unique<std::thread>([this]() { this->io_thread_func(); });

  ZCOROUTINE_LOG_INFO("IoScheduler::start scheduler and IO thread started");
}

void IoScheduler::stop() {

  // 先停止基类调度器（会等待所有任务完成）
  Scheduler::stop();

  // 唤醒IO线程
  wake_up();

  // 等待IO线程退出
  if (io_thread_ && io_thread_->joinable()) {
    io_thread_->join();
    ZCOROUTINE_LOG_DEBUG("IoScheduler::stop IO thread joined");
  }

  ZCOROUTINE_LOG_INFO("IoScheduler::stop stopped successfully");
}

int IoScheduler::add_event(int fd, FdContext::Event event,
                           std::function<void()> callback) {
  ZCOROUTINE_LOG_DEBUG(
      "IoScheduler::add_event fd={}, event={}, has_callback={}", fd,
      FdContext::event_to_string(event), callback != nullptr);

  FdContext::ptr fd_ctx = get_fd_context(fd, true);
  if (!fd_ctx) {
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::add_event failed to get FdContext, fd={}", fd);
    return -1;
  }

  // 设置回调或协程
  FdContext::EventContext &event_ctx = fd_ctx->get_event_context(event);
  if (callback) {
    event_ctx.callback = callback;
  } else {
    // 如果没有回调，使用当前协程
    auto current_fiber = Fiber::get_this();
    event_ctx.fiber = current_fiber;
  }
  // 记录该事件归属的调度器：即使在非调度线程触发 cancel/close，也能投递回正确的 Scheduler。
  event_ctx.scheduler = this;

  // 添加事件到FdContext
  // 注意：FdContext::add_event 在事件已存在时会直接返回当前 events_。
  // 因此这里必须基于“添加前”的 old_events 来决定 epoll_ctl 的 op，
  // 否则会对已在 epoll 中的 fd 误做 EPOLL_CTL_ADD（EEXIST）并错误回滚。
  const int old_events = fd_ctx->events();
  const int new_events = fd_ctx->add_event(event);

  // 更新epoll
  const int op = (old_events == FdContext::kNone) ? EPOLL_CTL_ADD
                                                  : EPOLL_CTL_MOD;
  int ret = 0;
  if (op == EPOLL_CTL_ADD) {
    ret = epoll_poller_->add_event(fd, new_events, fd_ctx.get());
  } else {
    ret = epoll_poller_->mod_event(fd, new_events, fd_ctx.get());
  }

  if (ret < 0) {
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::add_event epoll operation failed, fd={}, event={}, op={}",
        fd, FdContext::event_to_string(event),
        op == EPOLL_CTL_ADD ? "ADD" : "MOD");
    // 只在本次确实“新添加”了该事件时才回滚；
    // 若该事件原本就存在，回滚会把已有事件错误删除。
    if (!(old_events & event)) {
      fd_ctx->del_event(event);
    }
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG(
      "IoScheduler::add_event success, fd={}, event={}, new_events={}", fd,
      FdContext::event_to_string(event), new_events);
  return 0;
}

int IoScheduler::del_event(int fd, FdContext::Event event) {
  ZCOROUTINE_LOG_DEBUG("IoScheduler::del_event fd={}, event={}", fd,
                       FdContext::event_to_string(event));

  auto self = this;
  FdContext::ptr fd_ctx = self->get_fd_context(fd, false);
  if (!fd_ctx) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::del_event FdContext not found, fd={}",
                         fd);
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
    // 收尾/close 竞态：fd 可能已关闭(EBADF)或已不在 epoll 中(ENOENT)
    if (errno == EBADF || errno == ENOENT) {
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::del_event benign epoll failure, fd={}, errno={}", fd,
          errno);
      return 0;
    }
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::del_event epoll operation failed, fd={}, errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG(
      "IoScheduler::del_event success, fd={}, event={}, remaining_events={}",
      fd, FdContext::event_to_string(event), new_events);
  return 0;
}

int IoScheduler::cancel_event(int fd, FdContext::Event event) {
  ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_event fd={}, event={}", fd,
                       FdContext::event_to_string(event));

  auto self = this;
  FdContext::ptr fd_ctx = self->get_fd_context(fd, false);
  if (!fd_ctx) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_event FdContext not found, fd={}",
                         fd);
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
    if (errno == EBADF || errno == ENOENT) {
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::cancel_event benign epoll failure, fd={}, errno={}",
          fd, errno);
      return 0;
    }
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::cancel_event epoll operation failed, fd={}, errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG(
      "IoScheduler::cancel_event success, fd={}, event={}, remaining_events={}",
      fd, FdContext::event_to_string(event), new_events);
  return 0;
}

int IoScheduler::cancel_all(int fd) {
  ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_all fd={}", fd);

  auto self = this;
  FdContext::ptr fd_ctx = self->get_fd_context(fd, false);
  if (!fd_ctx) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_all FdContext not found, fd={}",
                         fd);
    return 0;
  }

  // 先检查是否有事件注册，避免重复删除 epoll 事件导致 ENOENT 错误
  int old_events = fd_ctx->events();
  if (old_events == FdContext::kNone) {
    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::cancel_all no events registered, fd={}", fd);
    return 0;
  }

  // 取消所有事件
  fd_ctx->cancel_all();

  // 从 epoll 中删除
  int ret = epoll_poller_->del_event(fd);
  if (ret < 0) {
    if (errno == EBADF || errno == ENOENT) {
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::cancel_all benign epoll failure, fd={}, errno={}", fd,
          errno);
      return 0;
    }
    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::cancel_all epoll del_event failed, fd={}, errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG("IoScheduler::cancel_all success, fd={}", fd);
  return 0;
}

Timer::ptr IoScheduler::add_timer(uint64_t timeout,
                                  std::function<void()> callback,
                                  bool recurring) {
  ZCOROUTINE_LOG_DEBUG("IoScheduler::add_timer timeout={}ms, recurring={}",
                       timeout, recurring);
  return timer_manager_->add_timer(timeout, std::move(callback), recurring);
}

Timer::ptr IoScheduler::add_condition_timer(uint64_t timeout,
                                            std::function<void()> callback,
                                            std::weak_ptr<void> weak_cond,
                                            bool recurring) {
  ZCOROUTINE_LOG_DEBUG(
      "IoScheduler::add_condition_timer timeout={}ms, recurring={}", timeout,
      recurring);
  // 直接调用 TimerManager，它会通过回调自动唤醒IO线程
  return timer_manager_->add_condition_timer(timeout, std::move(callback),
                                             std::move(weak_cond), recurring);
}

void IoScheduler::trigger_event(int fd, FdContext::Event event) {
  FdContext::ptr fd_ctx = get_fd_context(fd, false);
  if (!fd_ctx) {
    ZCOROUTINE_LOG_WARN("IoScheduler::trigger_event FdContext not found, fd={}",
                        fd);
    return;
  }

  ZCOROUTINE_LOG_DEBUG("IoScheduler::trigger_event fd={}, event={}", fd,
                       FdContext::event_to_string(event));

  // 线程安全地取出并清空上下文，同时清除事件位
  auto popped = fd_ctx->pop_event(event);
  if (!popped.had_event) {
    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::trigger_event event not registered (likely race), fd={}, event={}",
        fd, FdContext::event_to_string(event));
    return;
  }

  // 先更新 epoll，避免回调/协程中 re-arm 后被后续 DEL/MOD 覆盖
  int ret = 0;
  if (popped.remaining_events == FdContext::kNone) {
    ret = epoll_poller_->del_event(fd_ctx->fd());
  } else {
    ret = epoll_poller_->mod_event(fd_ctx->fd(), popped.remaining_events,
                                   fd_ctx.get());
  }
  if (ret < 0) {
    if (errno != EBADF && errno != ENOENT) {
      ZCOROUTINE_LOG_ERROR(
          "IoScheduler::trigger_event epoll operation failed, fd={}, errno={}",
          fd, errno);
    } else {
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::trigger_event benign epoll failure, fd={}, errno={}",
          fd, errno);
    }
  }

  // 停止过程中不再调度新任务，避免退出阶段被事件/回调持续延长
  if (stopping_.load(std::memory_order_relaxed)) {
    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::trigger_event scheduler stopping, skip scheduling");
    return;
  }
  
  // 最后触发回调或调度协程（epoll已更新，回调中可以安全地重新注册）
  if (popped.callback) {
    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::trigger_event executing callback: fd={}, event={}", fd,
        FdContext::event_to_string(event));
    schedule(std::move(popped.callback));
  } else if (popped.fiber) {
    ZCOROUTINE_LOG_DEBUG("IoScheduler::trigger_event scheduling fiber: fd={}, "
                         "event={}, fiber_id={}",
                         fd, FdContext::event_to_string(event),
                         popped.fiber->id());
    schedule(std::move(popped.fiber));
  } else {
    // event 位存在但上下文为空，通常是上层误用 add_event(nullptr) 且不在 fiber 内
    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::trigger_event empty context: fd={}, event={}", fd,
        FdContext::event_to_string(event));
  }
}

void IoScheduler::io_thread_func() {
  ZCOROUTINE_LOG_INFO("IoScheduler::io_thread_func IO thread started");

  std::vector<epoll_event> events;
  events.reserve(256); // 预分配容量，减少重新分配

  // 动态timeout优化参数
  static constexpr int kMinTimeout = 1;     // 最小超时 1ms
  static constexpr int kMaxTimeout = 5000;  // 最大超时 5s
  static constexpr int kIdleThreshold = 10; // 空闲阈值
  int idle_count = 0;

  while (!stopping_.load(std::memory_order_relaxed)) {
    // 获取下一个定时器超时时间
    int timeout = timer_manager_->get_next_timeout();

    // 动态调整timeout：根据负载情况调整
    if (timeout < 0) {
      // 没有定时器，根据空闲程度动态调整
      timeout = (idle_count > kIdleThreshold) ? kMaxTimeout : 100;
    } else {
      // 限制在合理范围内
      timeout = std::max(kMinTimeout, std::min(timeout, kMaxTimeout));
    }

    ZCOROUTINE_LOG_DEBUG(
        "IoScheduler::io_thread_func waiting for events, timeout={}ms",
        timeout);

    // 等待IO事件
    int nfds = epoll_poller_->wait(timeout, events);

    if (nfds < 0) {
      ZCOROUTINE_LOG_ERROR(
          "IoScheduler::io_thread_func epoll_wait failed, errno={}, error={}",
          errno, strerror(errno));
      continue;
    }

    // 动态调整空闲计数
    if (nfds == 0) {
      ++idle_count;
    } else {
      idle_count = 0; // 有事件时重置
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::io_thread_func epoll_wait returned nfds={}", nfds);
    }

    // 处理就绪事件
    for (int i = 0; i < nfds; ++i) {
      epoll_event &ev = events[i];

      // 检查是否是唤醒事件
      if (ev.data.ptr == nullptr) {
        char dummy[256];
        while (read(wake_fd_[0], dummy, sizeof(dummy)) > 0) {
        }
        ZCOROUTINE_LOG_DEBUG(
            "IoScheduler::io_thread_func wake up event received");
        continue;
      }

      // 处理IO事件
      auto *fd_ctx = static_cast<FdContext *>(ev.data.ptr);
      int fd = fd_ctx->fd();

      uint32_t ready_events = ev.events;
      if (ready_events & (EPOLLERR | EPOLLHUP)) {
        ZCOROUTINE_LOG_DEBUG(
            "IoScheduler::io_thread_func error/hup event, fd={}", fd);
        // 错误/挂起时，通常读写都会就绪；统一转成 IN/OUT 处理
        ready_events |= (EPOLLIN | EPOLLOUT);
      }

      if (ready_events & EPOLLIN) {
        ZCOROUTINE_LOG_DEBUG(
            "IoScheduler::io_thread_func triggering READ event, fd={}", fd);
        trigger_event(fd, FdContext::kRead);
      }
      if (ready_events & EPOLLOUT) {
        ZCOROUTINE_LOG_DEBUG(
            "IoScheduler::io_thread_func triggering WRITE event, fd={}", fd);
        trigger_event(fd, FdContext::kWrite);
      }
    }

    // 处理超时定时器
    auto expired_cbs = timer_manager_->list_expired_callbacks();
    if (!expired_cbs.empty()) {
      ZCOROUTINE_LOG_DEBUG(
          "IoScheduler::io_thread_func processing {} expired timers",
          expired_cbs.size());
    }
    for (const auto &cb : expired_cbs) {
      Scheduler::schedule(cb);
    }
  }

  ZCOROUTINE_LOG_INFO("IoScheduler::io_thread_func IO thread exiting");
}

void IoScheduler::wake_up() const {
  char dummy = 'W';
  while (true) {
    ssize_t n = write(wake_fd_[1], &dummy, 1);
    if (n == 1) {
      return;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    // 管道已满时，意味着读端在 epoll 中已就绪或将很快就绪；此时无需报错，直接返回。
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }

    ZCOROUTINE_LOG_ERROR(
        "IoScheduler::wake_up write failed, errno={}, error={}", errno,
        strerror(errno));
    return;
  }
}

IoScheduler *IoScheduler::get_this() {
  return dynamic_cast<IoScheduler *>(Scheduler::get_this());
}
} // namespace zcoroutine
