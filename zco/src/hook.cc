#include "zco/hook.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "zco/io_event.h"
#include "zco/zco_log.h"

namespace zco {
namespace {

struct FdMetadata {
    bool timeout_cached;
    uint32_t recv_timeout_ms;
    uint32_t send_timeout_ms;

    FdMetadata()
        : timeout_cached(false), recv_timeout_ms(kInfiniteTimeoutMs),
          send_timeout_ms(kInfiniteTimeoutMs) {}
};

class FdMetadataStore {
  public:
    FdMetadataStore() : shards_() {}

    bool try_get(int fd, FdMetadata *out) const {
        if (fd < 0 || !out) {
            return false;
        }

        Shard &shard = shard_for(fd);
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.items.find(fd);
        if (it == shard.items.end()) {
            return false;
        }

        *out = it->second;
        return true;
    }

    bool is_timeout_cached(int fd) const {
        FdMetadata metadata;
        return try_get(fd, &metadata) && metadata.timeout_cached;
    }

    void upsert(int fd, const FdMetadata &metadata) {
        if (fd < 0) {
            return;
        }

        Shard &shard = shard_for(fd);
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.items[fd] = metadata;
    }

    void erase(int fd) {
        if (fd < 0) {
            return;
        }

        Shard &shard = shard_for(fd);
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.items.erase(fd);
    }

  private:
    struct Shard {
        std::mutex mutex;
        std::unordered_map<int, FdMetadata> items;
    };

    static constexpr size_t kShardCount = 16;

    static size_t shard_index(int fd) {
        return static_cast<size_t>(fd) & (kShardCount - 1);
    }

    Shard &shard_for(int fd) { return shards_[shard_index(fd)]; }

    Shard &shard_for(int fd) const {
        return const_cast<FdMetadataStore *>(this)->shard_for(fd);
    }

    std::array<Shard, kShardCount> shards_;
};

FdMetadataStore g_fd_metadata_store;

bool is_retryable_errno(int err) { return err == EAGAIN || err == EWOULDBLOCK; }

bool require_coroutine_context(const char *func_name) {
    if (in_coroutine()) {
        return true;
    }
    errno = EPERM;
    ZCO_LOG_FATAL("{} contract violation: must be called in coroutine context",
                  func_name);
    return false;
}

bool force_nonblocking_fd(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if ((flags & O_NONBLOCK) != 0) {
        return true;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_cloexec_if_possible(int fd) {
    const int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) != 0) {
        return;
    }
    (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
}

bool require_nonblocking_fd(const char *func_name, int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if ((flags & O_NONBLOCK) != 0) {
        return true;
    }

    errno = EINVAL;
    ZCO_LOG_FATAL("{} contract violation: fd must be non-blocking", func_name);
    return false;
}

uint32_t timeval_to_milliseconds(const timeval &timeout) {
    if (timeout.tv_sec <= 0 && timeout.tv_usec <= 0) {
        return kInfiniteTimeoutMs;
    }

    const uint64_t total_us =
        static_cast<uint64_t>(timeout.tv_sec) * 1000000ULL +
        static_cast<uint64_t>(timeout.tv_usec);
    if (total_us == 0) {
        return kInfiniteTimeoutMs;
    }

    return static_cast<uint32_t>((total_us + 999ULL) / 1000ULL);
}

uint32_t load_socket_timeout_ms(int fd, int optname) {
    timeval timeout;
    std::memset(&timeout, 0, sizeof(timeout));
    socklen_t len = static_cast<socklen_t>(sizeof(timeout));
    if (::getsockopt(fd, SOL_SOCKET, optname, &timeout, &len) != 0) {
        return kInfiniteTimeoutMs;
    }

    return timeval_to_milliseconds(timeout);
}

void ensure_timeout_cached(int fd) {
    if (g_fd_metadata_store.is_timeout_cached(fd)) {
        return;
    }

    FdMetadata metadata;
    metadata.recv_timeout_ms = load_socket_timeout_ms(fd, SO_RCVTIMEO);
    metadata.send_timeout_ms = load_socket_timeout_ms(fd, SO_SNDTIMEO);
    metadata.timeout_cached = true;
    g_fd_metadata_store.upsert(fd, metadata);
}

uint32_t resolve_timeout_ms(int fd, uint32_t requested_timeout_ms,
                            bool is_read) {
    if (requested_timeout_ms != kInfiniteTimeoutMs) {
        return requested_timeout_ms;
    }

    ensure_timeout_cached(fd);

    FdMetadata metadata;
    if (!g_fd_metadata_store.try_get(fd, &metadata)) {
        return requested_timeout_ms;
    }

    return is_read ? metadata.recv_timeout_ms : metadata.send_timeout_ms;
}

void sync_fd_metadata_on_dup_impl(int from_fd, int to_fd) {
    if (to_fd < 0) {
        return;
    }

    FdMetadata source;
    const bool has_source = g_fd_metadata_store.try_get(from_fd, &source);

    FdMetadata target = has_source ? source : FdMetadata();
    if (!target.timeout_cached) {
        target.recv_timeout_ms = load_socket_timeout_ms(to_fd, SO_RCVTIMEO);
        target.send_timeout_ms = load_socket_timeout_ms(to_fd, SO_SNDTIMEO);
        target.timeout_cached = true;
    }

    g_fd_metadata_store.upsert(to_fd, target);
}

void refresh_timeout_cache_after_setsockopt(int fd, int level, int option,
                                            const void *option_value,
                                            socklen_t option_len) {
    if (level != SOL_SOCKET ||
        (option != SO_RCVTIMEO && option != SO_SNDTIMEO)) {
        return;
    }

    ensure_timeout_cached(fd);

    FdMetadata metadata;
    if (!g_fd_metadata_store.try_get(fd, &metadata)) {
        metadata = FdMetadata();
        metadata.timeout_cached = true;
    }

    const bool has_timeval =
        option_value != nullptr &&
        option_len >= static_cast<socklen_t>(sizeof(timeval));
    if (option == SO_RCVTIMEO) {
        metadata.recv_timeout_ms =
            has_timeval ? timeval_to_milliseconds(
                              *static_cast<const timeval *>(option_value))
                        : load_socket_timeout_ms(fd, SO_RCVTIMEO);
    } else {
        metadata.send_timeout_ms =
            has_timeval ? timeval_to_milliseconds(
                              *static_cast<const timeval *>(option_value))
                        : load_socket_timeout_ms(fd, SO_SNDTIMEO);
    }

    metadata.timeout_cached = true;
    g_fd_metadata_store.upsert(fd, metadata);
}

uint32_t
remaining_timeout_ms(uint32_t total_timeout_ms,
                     const std::chrono::steady_clock::time_point &started_at) {
    if (total_timeout_ms == kInfiniteTimeoutMs) {
        return kInfiniteTimeoutMs;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    if (elapsed.count() >= static_cast<int64_t>(total_timeout_ms)) {
        errno = ETIMEDOUT;
        return 0;
    }

    return total_timeout_ms - static_cast<uint32_t>(elapsed.count());
}

int decode_shutdown_how(char how) {
    switch (how) {
    case 'r':
        return SHUT_RD;
    case 'w':
        return SHUT_WR;
    case 'b':
        return SHUT_RDWR;
    default:
        errno = EINVAL;
        return -1;
    }
}

template <typename Func> ssize_t retry_on_eintr(Func &&func) {
    while (true) {
        const ssize_t rc = static_cast<ssize_t>(func());
        if (rc >= 0) {
            return rc;
        }

        if (errno != EINTR) {
            return rc;
        }
    }
}

template <typename Func>
ssize_t run_io_loop(const char *func_name, int fd, IoEventType event_type,
                    uint32_t timeout_ms, bool is_read, Func &&func) {
    if (!require_coroutine_context(func_name) ||
        !require_nonblocking_fd(func_name, fd)) {
        return -1;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, is_read);
    const auto started_at = std::chrono::steady_clock::now();

    while (true) {
        const ssize_t rc = retry_on_eintr(func);
        if (rc >= 0) {
            return rc;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, event_type);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }
}

} // namespace

void co_sleep_for(uint32_t milliseconds) { sleep_for(milliseconds); }

int co_error() { return errno; }

void co_error(int error_code) { errno = error_code; }

void sync_fd_metadata_on_dup(int from_fd, int to_fd) {
    sync_fd_metadata_on_dup_impl(from_fd, to_fd);
}

void sync_fd_metadata_on_close(int fd) {
    if (fd < 0) {
        return;
    }

    g_fd_metadata_store.erase(fd);
}

int co_socket(int domain, int type, int protocol) {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    const int fd = static_cast<int>(retry_on_eintr([&]() -> ssize_t {
        return ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    }));
#else
    const int fd = static_cast<int>(retry_on_eintr(
        [&]() -> ssize_t { return ::socket(domain, type, protocol); }));
#endif

    if (fd < 0) {
        return -1;
    }

    if (!force_nonblocking_fd(fd)) {
        const int err = errno;
        (void)::close(fd);
        errno = err;
        return -1;
    }

    set_cloexec_if_possible(fd);
    ensure_timeout_cached(fd);
    return fd;
}

int co_tcp_socket(int domain) {
    return co_socket(domain, SOCK_STREAM, IPPROTO_TCP);
}

int co_udp_socket(int domain) {
    return co_socket(domain, SOCK_DGRAM, IPPROTO_UDP);
}

int co_dup(int oldfd) {
    const int newfd = static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::dup(oldfd); }));
    if (newfd >= 0) {
        sync_fd_metadata_on_dup(oldfd, newfd);
    }
    return newfd;
}

int co_dup2(int oldfd, int newfd) {
    const int rc = static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::dup2(oldfd, newfd); }));
    if (rc >= 0) {
        sync_fd_metadata_on_dup(oldfd, rc);
    }
    return rc;
}

int co_dup3(int oldfd, int newfd, int flags) {
#if defined(__linux__)
    const int rc = static_cast<int>(retry_on_eintr(
        [&]() -> ssize_t { return ::dup3(oldfd, newfd, flags); }));
    if (rc >= 0) {
        sync_fd_metadata_on_dup(oldfd, rc);
    }
    return rc;
#else
    (void)oldfd;
    (void)newfd;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

int co_close(int fd, uint32_t delay_ms) {
    if (delay_ms > 0 && in_coroutine()) {
        co_sleep_for(delay_ms);
    }

    sync_fd_metadata_on_close(fd);
    return static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::close(fd); }));
}

int co_reset_tcp_socket(int fd, uint32_t delay_ms) {
    linger option;
    option.l_onoff = 1;
    option.l_linger = 0;
    if (co_setsockopt(fd, SOL_SOCKET, SO_LINGER, &option,
                      static_cast<socklen_t>(sizeof(option))) != 0) {
        return -1;
    }

    return co_close(fd, delay_ms);
}

int co_shutdown(int fd, char how) {
    const int shutdown_how = decode_shutdown_how(how);
    if (shutdown_how < 0) {
        return -1;
    }

    return static_cast<int>(retry_on_eintr(
        [&]() -> ssize_t { return ::shutdown(fd, shutdown_how); }));
}

int co_bind(int fd, const struct sockaddr *address, socklen_t address_len) {
    return static_cast<int>(retry_on_eintr(
        [&]() -> ssize_t { return ::bind(fd, address, address_len); }));
}

int co_listen(int fd, int backlog) {
    return static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::listen(fd, backlog); }));
}

int co_getsockopt(int fd, int level, int option, void *option_value,
                  socklen_t *option_len) {
    return static_cast<int>(retry_on_eintr([&]() -> ssize_t {
        return ::getsockopt(fd, level, option, option_value, option_len);
    }));
}

int co_setsockopt(int fd, int level, int option, const void *option_value,
                  socklen_t option_len) {
    const int rc = static_cast<int>(retry_on_eintr([&]() -> ssize_t {
        return ::setsockopt(fd, level, option, option_value, option_len);
    }));
    if (rc == 0) {
        refresh_timeout_cache_after_setsockopt(fd, level, option, option_value,
                                               option_len);
    }
    return rc;
}

void co_set_nonblock(int fd) {
    if (!force_nonblocking_fd(fd)) {
        ZCO_LOG_WARN("co_set_nonblock failed, fd={}, errno={}", fd, errno);
    }
}

void co_set_cloexec(int fd) { set_cloexec_if_possible(fd); }

void co_set_reuseaddr(int fd) {
    const int enabled = 1;
    (void)co_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                        sizeof(enabled));
}

void co_set_send_buffer_size(int fd, int bytes) {
    (void)co_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

void co_set_recv_buffer_size(int fd, int bytes) {
    (void)co_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

void co_set_tcp_keepalive(int fd) {
    const int enabled = 1;
    (void)co_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled,
                        sizeof(enabled));
}

void co_set_tcp_nodelay(int fd) {
    const int enabled = 1;
    (void)co_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                        sizeof(enabled));
}

ssize_t co_read(int fd, void *buffer, size_t count, uint32_t timeout_ms) {
    return run_io_loop("co_read", fd, IoEventType::kRead, timeout_ms, true,
                       [&]() -> ssize_t { return ::read(fd, buffer, count); });
}

ssize_t co_write(int fd, const void *buffer, size_t count,
                 uint32_t timeout_ms) {
    return run_io_loop("co_write", fd, IoEventType::kWrite, timeout_ms, false,
                       [&]() -> ssize_t { return ::write(fd, buffer, count); });
}

ssize_t co_readv(int fd, const struct iovec *iov, int iovcnt,
                 uint32_t timeout_ms) {
    return run_io_loop("co_readv", fd, IoEventType::kRead, timeout_ms, true,
                       [&]() -> ssize_t { return ::readv(fd, iov, iovcnt); });
}

ssize_t co_writev(int fd, const struct iovec *iov, int iovcnt,
                  uint32_t timeout_ms) {
    return run_io_loop("co_writev", fd, IoEventType::kWrite, timeout_ms, false,
                       [&]() -> ssize_t { return ::writev(fd, iov, iovcnt); });
}

ssize_t co_recv(int fd, void *buffer, size_t count, int flags,
                uint32_t timeout_ms) {
    return run_io_loop(
        "co_recv", fd, IoEventType::kRead, timeout_ms, true,
        [&]() -> ssize_t { return ::recv(fd, buffer, count, flags); });
}

ssize_t co_recvn(int fd, void *buffer, size_t count, int flags,
                 uint32_t timeout_ms) {
    if (!require_coroutine_context("co_recvn") ||
        !require_nonblocking_fd("co_recvn", fd)) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, true);
    const auto started_at = std::chrono::steady_clock::now();

    size_t received = 0;
    char *current = static_cast<char *>(buffer);
    while (received < count) {
        const ssize_t rc = retry_on_eintr([&]() -> ssize_t {
            return ::recv(fd, current, count - received, flags);
        });
        if (rc > 0) {
            received += static_cast<size_t>(rc);
            current += rc;
            continue;
        }

        if (rc == 0) {
            return 0;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kRead);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }

    return static_cast<ssize_t>(count);
}

ssize_t co_send(int fd, const void *buffer, size_t count, int flags,
                uint32_t timeout_ms) {
    if (!require_coroutine_context("co_send") ||
        !require_nonblocking_fd("co_send", fd)) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, false);
    const auto started_at = std::chrono::steady_clock::now();

    size_t sent = 0;
    const char *current = static_cast<const char *>(buffer);
    while (sent < count) {
        const ssize_t rc = retry_on_eintr([&]() -> ssize_t {
            return ::send(fd, current, count - sent, flags);
        });
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            current += rc;
            continue;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kWrite);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }

    return static_cast<ssize_t>(count);
}

ssize_t co_recvfrom(int fd, void *buffer, size_t count, int flags,
                    struct sockaddr *address, socklen_t *address_len,
                    uint32_t timeout_ms) {
    return run_io_loop("co_recvfrom", fd, IoEventType::kRead, timeout_ms, true,
                       [&]() -> ssize_t {
                           return ::recvfrom(fd, buffer, count, flags, address,
                                             address_len);
                       });
}

ssize_t co_sendto(int fd, const void *buffer, size_t count, int flags,
                  const struct sockaddr *address, socklen_t address_len,
                  uint32_t timeout_ms) {
    if (address == nullptr && address_len == 0) {
        return co_send(fd, buffer, count, flags, timeout_ms);
    }

    if (!require_coroutine_context("co_sendto") ||
        !require_nonblocking_fd("co_sendto", fd)) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, false);
    const auto started_at = std::chrono::steady_clock::now();

    size_t sent = 0;
    const char *current = static_cast<const char *>(buffer);
    while (sent < count) {
        const ssize_t rc = retry_on_eintr([&]() -> ssize_t {
            return ::sendto(fd, current, count - sent, flags, address,
                            address_len);
        });
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            current += rc;
            continue;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kWrite);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }

    return static_cast<ssize_t>(count);
}

int co_connect(int fd, const struct sockaddr *address, socklen_t address_len,
               uint32_t timeout_ms) {
    if (!require_coroutine_context("co_connect") ||
        !require_nonblocking_fd("co_connect", fd)) {
        return -1;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, false);
    const auto started_at = std::chrono::steady_clock::now();

    while (true) {
        const int rc = static_cast<int>(retry_on_eintr(
            [&]() -> ssize_t { return ::connect(fd, address, address_len); }));
        if (rc == 0 || errno == EISCONN) {
            return 0;
        }

        if (errno != EINPROGRESS && errno != EALREADY &&
            !is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kWrite);
        if (!io_event.wait(wait_timeout_ms)) {
            if (errno == 0) {
                errno = ETIMEDOUT;
            }
            return -1;
        }

        int socket_error = 0;
        socklen_t socket_error_len =
            static_cast<socklen_t>(sizeof(socket_error));
        if (co_getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                          &socket_error_len) != 0) {
            return -1;
        }

        if (socket_error == 0 || socket_error == EISCONN) {
            return 0;
        }

        errno = socket_error;
        if (errno != EINPROGRESS && errno != EALREADY &&
            !is_retryable_errno(errno)) {
            return -1;
        }
    }
}

int co_accept(int fd, struct sockaddr *address, socklen_t *address_len,
              uint32_t timeout_ms) {
    if (!require_coroutine_context("co_accept") ||
        !require_nonblocking_fd("co_accept", fd)) {
        return -1;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, true);
    const auto started_at = std::chrono::steady_clock::now();

    while (true) {
        const int accepted_fd = static_cast<int>(retry_on_eintr(
            [&]() -> ssize_t { return ::accept(fd, address, address_len); }));
        if (accepted_fd >= 0) {
            if (!force_nonblocking_fd(accepted_fd)) {
                const int err = errno;
                (void)::close(accepted_fd);
                errno = err;
                return -1;
            }
            set_cloexec_if_possible(accepted_fd);
            sync_fd_metadata_on_dup(fd, accepted_fd);
            return accepted_fd;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kRead);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }
}

int co_accept4(int fd, struct sockaddr *address, socklen_t *address_len,
               int flags, uint32_t timeout_ms) {
#if defined(__linux__)
    if (!require_coroutine_context("co_accept4") ||
        !require_nonblocking_fd("co_accept4", fd)) {
        return -1;
    }

    const uint32_t effective_timeout_ms =
        resolve_timeout_ms(fd, timeout_ms, true);
    const auto started_at = std::chrono::steady_clock::now();

    int syscall_flags = flags;
#ifdef SOCK_NONBLOCK
    syscall_flags |= SOCK_NONBLOCK;
#endif
#ifdef SOCK_CLOEXEC
    syscall_flags |= SOCK_CLOEXEC;
#endif

    while (true) {
        const int accepted_fd =
            static_cast<int>(retry_on_eintr([&]() -> ssize_t {
                return ::accept4(fd, address, address_len, syscall_flags);
            }));
        if (accepted_fd >= 0) {
            sync_fd_metadata_on_dup(fd, accepted_fd);
            return accepted_fd;
        }

        if (errno == ENOSYS) {
            break;
        }

        if (!is_retryable_errno(errno)) {
            return -1;
        }

        const uint32_t wait_timeout_ms =
            remaining_timeout_ms(effective_timeout_ms, started_at);
        if (wait_timeout_ms == 0) {
            return -1;
        }

        IoEvent io_event(fd, IoEventType::kRead);
        if (!io_event.wait(wait_timeout_ms)) {
            return -1;
        }
    }
#endif

    return co_accept(fd, address, address_len, timeout_ms);
}

} // namespace zco
