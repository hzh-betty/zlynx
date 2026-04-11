#include "zco/io_event.h"

#include <errno.h>
#include <sys/epoll.h>

#include "zco/internal/runtime_manager.h"
#include "zco/zco_log.h"

namespace zco {

// IoEvent 在 Linux 下封装 epoll 等待语义：
// 1) 将高层读写事件映射到 EPOLLIN/EPOLLOUT。
// 2) 必须在协程上下文走 wait_fd 挂起协程。
// 3) 不再提供线程 poll fallback。

IoEvent::IoEvent(int fd, IoEventType event_type)
    : fd_(fd), event_type_(event_type), added_(false) {}

IoEvent::~IoEvent() = default;

bool IoEvent::wait(uint32_t milliseconds) {
    ZCO_LOG_DEBUG("io_event wait start, fd={}, event_type={}, timeout_ms={}",
                  fd_, static_cast<uint32_t>(event_type_), milliseconds);
    if (fd_ < 0) {
        const int saved_errno = EBADF;
        errno = saved_errno;
        ZCO_LOG_ERROR("io_event wait failed, invalid fd={}", fd_);
        errno = saved_errno;
        return false;
    }

    uint32_t epoll_events = 0;
    switch (event_type_) {
    case IoEventType::kRead:
        epoll_events = EPOLLIN;
        break;
    case IoEventType::kWrite:
        epoll_events = EPOLLOUT;
        break;
    default:
        const int saved_errno = EINVAL;
        errno = saved_errno;
        ZCO_LOG_ERROR("io_event wait failed, invalid event_type={}, fd={}",
                      static_cast<uint32_t>(event_type_), fd_);
        errno = saved_errno;
        return false;
    }

    // IoEvent 只做事件语义映射，不直接管理 epoll 生命周期。
    // 具体等待/挂起行为由 runtime_manager::wait_fd 分发到协程路径。
    // added_ 当前用于记录 wait 生命周期，可在后续扩展中用于调试或资源追踪。
    added_ = true;
    const bool ok = wait_fd(fd_, epoll_events, milliseconds);
    // wait_fd 返回 false 但 errno 未设置时，若当前协程超时，则补充 ETIMEDOUT。
    if (!ok && errno == 0 && timeout()) {
        errno = ETIMEDOUT;
    }
    const int saved_errno = errno;
    ZCO_LOG_DEBUG("io_event wait result, fd={}, ok={}, errno={}", fd_, ok,
                  saved_errno);
    errno = saved_errno;
    return ok;
}

} // namespace zco
