#include "zcoroutine/internal/epoller.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <utility>
#include <vector>

#include "zcoroutine/log.h"

namespace zcoroutine {

// Epoller 封装了调度线程的 IO 多路复用基础设施：
// - 一个 epoll 实例管理所有 fd 等待。
// - 一个 eventfd 用于跨线程唤醒 epoll_wait。
// - 每个 fd 维护读/写两个等待槽位，避免相互覆盖。

namespace {

constexpr int kMaxEpollEvents = 64;

} // namespace

Epoller::Epoller()
    : epoll_fd_(-1), wake_fd_(-1), wake_pending_(false), waiter_mutex_(),
      fd_wait_states_() {}

Epoller::~Epoller() { stop(); }

bool Epoller::start() {
    // Epoller 维护一个 eventfd，用于跨线程打断 epoll_wait。
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epoll_fd_ < 0 || wake_fd_ < 0) {
        ZCOROUTINE_LOG_FATAL(
            "epoller init failed, epoll_fd={}, wake_fd={}, errno={}", epoll_fd_,
            wake_fd_, errno);
        stop();
        return false;
    }

    epoll_event wake_ev;
    ::memset(&wake_ev, 0, sizeof(wake_ev));
    wake_ev.events = EPOLLIN;
    wake_ev.data.fd = wake_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &wake_ev) != 0) {
        ZCOROUTINE_LOG_FATAL(
            "epoller register wake fd failed, wake_fd={}, errno={}", wake_fd_,
            errno);
        stop();
        return false;
    }

    ZCOROUTINE_LOG_INFO("epoller started, epoll_fd={}, wake_fd={}", epoll_fd_,
                        wake_fd_);
    return true;
}

void Epoller::stop() {
    {
        std::lock_guard<std::mutex> lock(waiter_mutex_);
        fd_wait_states_.clear();
    }

    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }

    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    ZCOROUTINE_LOG_INFO("epoller stopped");
}

void Epoller::wake() {
    if (wake_fd_ < 0) {
        return;
    }

    bool expected = false;
    if (!wake_pending_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return;
    }

    const uint64_t value = 1;
    const ssize_t rc = write(wake_fd_, &value, sizeof(value));
    if (rc < 0 && errno != EAGAIN) {
        wake_pending_.store(false, std::memory_order_release);
        ZCOROUTINE_LOG_WARN("epoller wake failed, wake_fd={}, errno={}",
                            wake_fd_, errno);
    }
}

bool Epoller::register_waiter(const std::shared_ptr<IoWaiter> &waiter) {
    if (epoll_fd_ < 0 || !waiter || waiter->fd < 0) {
        errno = EINVAL;
        return false;
    }

    const bool want_read = (waiter->events & EPOLLIN) != 0;
    const bool want_write = (waiter->events & EPOLLOUT) != 0;
    if (!want_read && !want_write) {
        errno = EINVAL;
        return false;
    }

    std::lock_guard<std::mutex> lock(waiter_mutex_);

    FdWaitState &state = fd_wait_states_[waiter->fd];
    FdWaitState old_state = state;

    if (want_read) {
        if (state.read_waiter && state.read_waiter.get() != waiter.get()) {
            errno = EBUSY;
            return false;
        }
        state.read_waiter = waiter;
    }

    if (want_write) {
        if (state.write_waiter && state.write_waiter.get() != waiter.get()) {
            errno = EBUSY;
            return false;
        }
        state.write_waiter = waiter;
    }

    if (update_interest_locked(waiter->fd, &state)) {
        return true;
    }

    state = old_state;
    if (!state.registered && !state.read_waiter && !state.write_waiter) {
        fd_wait_states_.erase(waiter->fd);
    }
    return false;
}

void Epoller::unregister_waiter(const std::shared_ptr<IoWaiter> &waiter) {
    if (epoll_fd_ < 0 || !waiter || waiter->fd < 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(waiter_mutex_);
    auto it = fd_wait_states_.find(waiter->fd);
    if (it == fd_wait_states_.end()) {
        return;
    }

    FdWaitState &state = it->second;
    bool changed = false;

    if ((waiter->events & EPOLLIN) && state.read_waiter.get() == waiter.get()) {
        state.read_waiter.reset();
        changed = true;
    }

    if ((waiter->events & EPOLLOUT) &&
        state.write_waiter.get() == waiter.get()) {
        state.write_waiter.reset();
        changed = true;
    }

    if (changed) {
        (void)update_interest_locked(waiter->fd, &state);
    }

    if (!state.registered && !state.read_waiter && !state.write_waiter) {
        fd_wait_states_.erase(it);
    }
}

bool Epoller::update_interest_locked(int fd, FdWaitState *state) {
    if (!state) {
        errno = EINVAL;
        return false;
    }

    uint32_t desired_events = 0;
    if (state->read_waiter &&
        state->read_waiter->active.load(std::memory_order_acquire)) {
        desired_events |= EPOLLIN;
    }
    if (state->write_waiter &&
        state->write_waiter->active.load(std::memory_order_acquire)) {
        desired_events |= EPOLLOUT;
    }

    if (desired_events == 0) {
        if (state->registered) {
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0 &&
                errno != ENOENT) {
                ZCOROUTINE_LOG_WARN("epoll del waiter failed, fd={}, errno={}",
                                    fd, errno);
                return false;
            }
        }

        state->registered = false;
        state->registered_events = 0;
        return true;
    }

    if (state->registered && state->registered_events == desired_events) {
        return true;
    }

    epoll_event ev;
    ::memset(&ev, 0, sizeof(ev));
    ev.events = desired_events;
    ev.data.fd = fd;

    int op = state->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    int rc = epoll_ctl(epoll_fd_, op, fd, &ev);
    if (rc != 0 && op == EPOLL_CTL_ADD && errno == EEXIST) {
        op = EPOLL_CTL_MOD;
        rc = epoll_ctl(epoll_fd_, op, fd, &ev);
    } else if (rc != 0 && op == EPOLL_CTL_MOD && errno == ENOENT) {
        op = EPOLL_CTL_ADD;
        rc = epoll_ctl(epoll_fd_, op, fd, &ev);
    }

    if (rc != 0) {
        ZCOROUTINE_LOG_WARN(
            "epoll add/mod waiter failed, fd={}, events={}, errno={}", fd,
            desired_events, errno);
        return false;
    }

    state->registered = true;
    state->registered_events = desired_events;
    return true;
}

void Epoller::wait_events(
    int timeout_ms,
    const std::function<void(const std::shared_ptr<IoWaiter> &waiter,
                             uint32_t ready_events)> &on_ready) {
    if (epoll_fd_ < 0) {
        return;
    }

    epoll_event events[kMaxEpollEvents];
    const int event_count =
        epoll_wait(epoll_fd_, events, kMaxEpollEvents, timeout_ms);
    if (event_count <= 0) {
        if (event_count < 0 && errno != EINTR) {
            ZCOROUTINE_LOG_WARN("epoll_wait failed, errno={}", errno);
        }
        return;
    }

    std::vector<std::pair<std::shared_ptr<IoWaiter>, uint32_t>> ready_waiters;
    ready_waiters.reserve(static_cast<size_t>(event_count) * 2);

    for (int i = 0; i < event_count; ++i) {
        if (events[i].data.fd == wake_fd_) {
            // wake_fd 事件只用于打断阻塞，不对应业务 fd。
            consume_wakeup_fd();
            continue;
        }

        const int fd = events[i].data.fd;
        const uint32_t ready_events = events[i].events;

        std::lock_guard<std::mutex> lock(waiter_mutex_);
        auto it = fd_wait_states_.find(fd);
        if (it == fd_wait_states_.end()) {
            continue;
        }

        FdWaitState &state = it->second;
        const bool read_ready =
            (ready_events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        const bool write_ready =
            (ready_events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0;

        if (read_ready && state.read_waiter) {
            ready_waiters.push_back(
                std::make_pair(state.read_waiter, ready_events));
            state.read_waiter.reset();
        }

        if (write_ready && state.write_waiter) {
            ready_waiters.push_back(
                std::make_pair(state.write_waiter, ready_events));
            state.write_waiter.reset();
        }

        (void)update_interest_locked(fd, &state);
        if (!state.registered && !state.read_waiter && !state.write_waiter) {
            fd_wait_states_.erase(it);
        }
    }

    if (on_ready) {
        for (size_t i = 0; i < ready_waiters.size(); ++i) {
            on_ready(ready_waiters[i].first, ready_waiters[i].second);
        }
    }

    if (event_count > 0) {
        ZCOROUTINE_LOG_DEBUG(
            "epoll wait returned events, count={}, timeout_ms={}", event_count,
            timeout_ms);
    }
}

void Epoller::consume_wakeup_fd() {
    if (wake_fd_ < 0) {
        return;
    }

    uint64_t value = 0;
    // 一次可能积累多个写入，循环读空避免下一轮 epoll 立即被同一事件触发。
    while (read(wake_fd_, &value, sizeof(value)) == sizeof(value)) {
    }
    wake_pending_.store(false, std::memory_order_release);
}

std::unique_ptr<Poller> create_default_poller() {
    return std::unique_ptr<Poller>(new Epoller());
}

} // namespace zcoroutine
