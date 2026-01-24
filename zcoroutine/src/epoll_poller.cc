#include "epoll_poller.h"

#include <unistd.h>

#include "zcoroutine_logger.h"

namespace zcoroutine {

EpollPoller::EpollPoller(size_t max_events)
    : epoll_fd_(-1), max_events_(max_events) {

  // 创建epoll实例
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    ZCOROUTINE_LOG_ERROR(
        "EpollPoller::EpollPoller epoll_create1 failed, errno={}", errno);
    return;
  }

  // 预分配事件数组
  events_.resize(max_events_);

  ZCOROUTINE_LOG_DEBUG("EpollPoller::EpollPoller epoll_fd={} max_events={}",
                       epoll_fd_, max_events_);
}

EpollPoller::~EpollPoller() {
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
    ZCOROUTINE_LOG_DEBUG("EpollPoller::~EpollPoller close epoll_fd={}",
                         epoll_fd_);
  }
}

int EpollPoller::add_event(int fd, int events, void *data) {
  epoll_event ev = {};
  ev.events = events | EPOLLET; // 边缘触发
  ev.data.ptr = data;

  int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
  if (ret < 0) {
    ZCOROUTINE_LOG_ERROR(
        "EpollPoller::add_event epoll_ctl ADD failed, fd={} errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG("EpollPoller::add_event fd={} events={}", fd, events);
  return 0;
}

int EpollPoller::mod_event(int fd, int events, void *data) {
  epoll_event ev = {};
  ev.events = events | EPOLLET; // 边缘触发
  ev.data.ptr = data;

  int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
  if (ret < 0) {
    ZCOROUTINE_LOG_ERROR(
        "EpollPoller::mod_event epoll_ctl MOD failed, fd={} errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG("EpollPoller::mod_event fd={} events={}", fd, events);
  return 0;
}

int EpollPoller::del_event(int fd) {
  int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  if (ret < 0) {
    // 竞态收尾时 fd 可能已关闭(EBADF)或已不在 epoll 中(ENOENT)，视为成功即可。
    if (errno == EBADF || errno == ENOENT) {
      ZCOROUTINE_LOG_DEBUG(
          "EpollPoller::del_event epoll_ctl DEL benign failure, fd={} errno={}",
          fd, errno);
      return 0;
    }
    ZCOROUTINE_LOG_ERROR(
        "EpollPoller::del_event epoll_ctl DEL failed, fd={} errno={}", fd,
        errno);
    return -1;
  }

  ZCOROUTINE_LOG_DEBUG("EpollPoller::del_event fd={}", fd);
  return 0;
}

int EpollPoller::wait(int timeout_ms, std::vector<epoll_event> &events) {
  int nfds = epoll_wait(epoll_fd_, &events_[0], static_cast<int>(max_events_),
                        timeout_ms);

  if (nfds < 0) {
    if (errno == EINTR) {
      // 被信号中断，返回0
      return 0;
    }
    ZCOROUTINE_LOG_ERROR("EpollPoller::wait epoll_wait failed, errno={}",
                         errno);
    return -1;
  }

  if (nfds > 0) {
    events.resize(nfds);
    std::copy(events_.begin(), events_.begin() + nfds, events.begin());
  } else {
    events.clear();
  }

  ZCOROUTINE_LOG_DEBUG("EpollPoller::wait nfds={} timeout={}", nfds,
                       timeout_ms);
  return nfds;
}

} // namespace zcoroutine
