#include "zcoroutine/hook.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "zcoroutine/io_event.h"
#include "zcoroutine/log.h"

namespace zcoroutine {
namespace {

// hook.cc 的最小封装策略：
// - 保持与系统调用一致的返回值与 errno 语义。
// - 在 EAGAIN/EWOULDBLOCK 时转为 IoEvent 等待，避免忙等。
// - 对 EINTR 做透明重试，减少调用方样板代码。
//
// 绝大多数 co_xxx 的执行模板都一致：
// 1) 临时把 fd 调成非阻塞。
// 2) 直接尝试系统调用。
// 3) 若因 EAGAIN/EWOULDBLOCK 失败，则等待对应 IO 事件。
// 4) 被唤醒后重试，直到成功或遇到不可恢复错误。

struct FdMetadata {
  bool user_nonblocking;
  bool timeout_cached;
  uint32_t recv_timeout_ms;
  uint32_t send_timeout_ms;

  FdMetadata()
      : user_nonblocking(false),
        timeout_cached(false),
        recv_timeout_ms(kInfiniteTimeoutMs),
        send_timeout_ms(kInfiniteTimeoutMs) {}
};

class FdMetadataStore {
 public:
  FdMetadataStore() : shards_() {}

  bool try_get(int fd, FdMetadata* out) const {
    if (fd < 0 || !out) {
      return false;
    }

    Shard& shard = shard_for(fd);
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

  void set_user_nonblocking_if_absent(int fd, bool user_nonblocking) {
    if (fd < 0) {
      return;
    }

    Shard& shard = shard_for(fd);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.items.find(fd);
    if (it == shard.items.end()) {
      shard.items.emplace(fd, FdMetadata()).first->second.user_nonblocking = user_nonblocking;
    }
  }

  void upsert(int fd, const FdMetadata& metadata) {
    if (fd < 0) {
      return;
    }

    Shard& shard = shard_for(fd);
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.items[fd] = metadata;
  }

  void erase(int fd) {
    if (fd < 0) {
      return;
    }

    Shard& shard = shard_for(fd);
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

  Shard& shard_for(int fd) {
    return shards_[shard_index(fd)];
  }

  Shard& shard_for(int fd) const {
    return const_cast<FdMetadataStore*>(this)->shard_for(fd);
  }

  std::array<Shard, kShardCount> shards_;
};

FdMetadataStore g_fd_metadata_store;

uint32_t timeval_to_milliseconds(const timeval& timeout) {
  if (timeout.tv_sec <= 0 && timeout.tv_usec <= 0) {
    return kInfiniteTimeoutMs;
  }

  const uint64_t total_us = static_cast<uint64_t>(timeout.tv_sec) * 1000000ULL +
                            static_cast<uint64_t>(timeout.tv_usec);
  if (total_us == 0) {
    return kInfiniteTimeoutMs;
  }
  return static_cast<uint32_t>((total_us + 999ULL) / 1000ULL);
}

uint32_t load_socket_timeout_ms(int fd, int optname) {
  timeval timeout;
  std::memset(&timeout, 0, sizeof(timeout));
  socklen_t len = sizeof(timeout);
  if (::getsockopt(fd, SOL_SOCKET, optname, &timeout, &len) != 0) {
    return kInfiniteTimeoutMs;
  }
  return timeval_to_milliseconds(timeout);
}

void seed_user_nonblocking_metadata(int fd, bool user_nonblocking) {
  g_fd_metadata_store.set_user_nonblocking_if_absent(fd, user_nonblocking);
}

bool get_user_nonblocking(int fd) {
  FdMetadata existing;
  if (g_fd_metadata_store.try_get(fd, &existing)) {
    return existing.user_nonblocking;
  }

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }

  const bool user_nonblocking = (flags & O_NONBLOCK) != 0;
  g_fd_metadata_store.set_user_nonblocking_if_absent(fd, user_nonblocking);
  return user_nonblocking;
}

void ensure_timeout_cached(int fd) {
  if (g_fd_metadata_store.is_timeout_cached(fd)) {
    return;
  }

  FdMetadata refreshed;
  refreshed.recv_timeout_ms = load_socket_timeout_ms(fd, SO_RCVTIMEO);
  refreshed.send_timeout_ms = load_socket_timeout_ms(fd, SO_SNDTIMEO);
  refreshed.timeout_cached = true;
  refreshed.user_nonblocking = get_user_nonblocking(fd);
  g_fd_metadata_store.upsert(fd, refreshed);
}

uint32_t resolve_timeout_ms(int fd, uint32_t requested_timeout_ms, bool is_read) {
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
  target.user_nonblocking = get_user_nonblocking(to_fd);
  if (!target.timeout_cached) {
    target.recv_timeout_ms = load_socket_timeout_ms(to_fd, SO_RCVTIMEO);
    target.send_timeout_ms = load_socket_timeout_ms(to_fd, SO_SNDTIMEO);
    target.timeout_cached = true;
  }

  g_fd_metadata_store.upsert(to_fd, target);
}

/**
 * @brief 文件描述符非阻塞模式守卫。
 * @details 进入作用域时可选开启 O_NONBLOCK，退出时恢复原始 flags。
 */
class FdNonBlockingGuard {
 public:
  /**
   * @brief 构造并尝试设置非阻塞。
   * @param fd 文件描述符。
   */
  explicit FdNonBlockingGuard(int fd, bool should_force_nonblocking)
      : fd_(fd), valid_(false), changed_(false), old_flags_(0) {
    if (!should_force_nonblocking) {
      return;
    }

    old_flags_ = fcntl(fd_, F_GETFL, 0);
    if (old_flags_ < 0) {
      return;
    }
    valid_ = true;
    if ((old_flags_ & O_NONBLOCK) != 0) {
      return;
    }

    // 在临时切换前写入用户语义，避免并发路径把内部临时 nonblock 误判为用户设置。
    seed_user_nonblocking_metadata(fd_, false);

    if (fcntl(fd_, F_SETFL, old_flags_ | O_NONBLOCK) == 0) {
      changed_ = true;
    }
  }

  /**
   * @brief 析构时恢复文件描述符状态。
   */
  ~FdNonBlockingGuard() {
    if (!valid_ || !changed_) {
      return;
    }
    (void)fcntl(fd_, F_SETFL, old_flags_);
  }

 private:
  int fd_;
  bool valid_;
  bool changed_;
  int old_flags_;
};

/**
 * @brief 可重试的系统调用循环助手。
 * @tparam Func 可调用对象类型。
 * @param func 调用函数。
 * @return 调用返回值。
 */
template <typename Func>
ssize_t retry_on_eintr(Func&& func) {
  // EINTR 代表被信号中断，不是业务失败，直接重试最符合调用方预期。
  while (true) {
    const ssize_t rc = func();
    if (rc >= 0) {
      return rc;
    }
    if (errno != EINTR) {
      return rc;
    }
  }
}

}  // namespace

void co_sleep_for(uint32_t milliseconds) { sleep_for(milliseconds); }

void sync_fd_metadata_on_dup(int from_fd, int to_fd) {
  sync_fd_metadata_on_dup_impl(from_fd, to_fd);
}

void sync_fd_metadata_on_close(int fd) {
  if (fd < 0) {
    return;
  }

  g_fd_metadata_store.erase(fd);
}

int co_dup(int oldfd) {
  const int newfd = static_cast<int>(retry_on_eintr([&]() -> ssize_t { return ::dup(oldfd); }));
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
  const int rc = static_cast<int>(
      retry_on_eintr([&]() -> ssize_t { return ::dup3(oldfd, newfd, flags); }));
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

int co_close(int fd) {
  sync_fd_metadata_on_close(fd);
  return static_cast<int>(retry_on_eintr([&]() -> ssize_t { return ::close(fd); }));
}

ssize_t co_read(int fd, void* buffer, size_t count, uint32_t timeout_ms) {
  // co_read 行为：先直接读；若暂不可读则等待可读事件再重试。
  ZCOROUTINE_LOG_DEBUG("co_read start, fd={}, count={}, timeout_ms={}", fd, count, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::read(fd, buffer, count); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_read failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_read wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_write(int fd, const void* buffer, size_t count, uint32_t timeout_ms) {
  // co_write 行为：先直接写；若暂不可写则等待可写事件再重试。
  ZCOROUTINE_LOG_DEBUG("co_write start, fd={}, count={}, timeout_ms={}", fd, count, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, false);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::write(fd, buffer, count); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_write failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kWrite);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_write wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_readv(int fd, const struct iovec* iov, int iovcnt, uint32_t timeout_ms) {
  ZCOROUTINE_LOG_DEBUG("co_readv start, fd={}, iovcnt={}, timeout_ms={}", fd, iovcnt, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::readv(fd, iov, iovcnt); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_readv failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_readv wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_writev(int fd, const struct iovec* iov, int iovcnt, uint32_t timeout_ms) {
  ZCOROUTINE_LOG_DEBUG("co_writev start, fd={}, iovcnt={}, timeout_ms={}", fd, iovcnt, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, false);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::writev(fd, iov, iovcnt); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_writev failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kWrite);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_writev wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_recv(int fd, void* buffer, size_t count, int flags, uint32_t timeout_ms) {
  // co_recv 与 co_read 类似，但保留 recv flags 语义。
  ZCOROUTINE_LOG_DEBUG("co_recv start, fd={}, count={}, flags={}, timeout_ms={}", fd, count, flags,
                       timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::recv(fd, buffer, count, flags); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_recv failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_recv wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_send(int fd, const void* buffer, size_t count, int flags, uint32_t timeout_ms) {
  // co_send 与 co_write 类似，但保留 send flags 语义。
  ZCOROUTINE_LOG_DEBUG("co_send start, fd={}, count={}, flags={}, timeout_ms={}", fd, count, flags,
                       timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, false);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr([&]() { return ::send(fd, buffer, count, flags); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_send failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kWrite);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_send wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_recvfrom(int fd,
                    void* buffer,
                    size_t count,
                    int flags,
                    struct sockaddr* address,
                    socklen_t* address_len,
                    uint32_t timeout_ms) {
  ZCOROUTINE_LOG_DEBUG("co_recvfrom start, fd={}, count={}, flags={}, timeout_ms={}", fd, count,
                       flags, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr(
        [&]() { return ::recvfrom(fd, buffer, count, flags, address, address_len); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_recvfrom failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_recvfrom wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

ssize_t co_sendto(int fd,
                  const void* buffer,
                  size_t count,
                  int flags,
                  const struct sockaddr* address,
                  socklen_t address_len,
                  uint32_t timeout_ms) {
  ZCOROUTINE_LOG_DEBUG("co_sendto start, fd={}, count={}, flags={}, timeout_ms={}", fd, count,
                       flags, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, false);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const ssize_t rc = retry_on_eintr(
        [&]() { return ::sendto(fd, buffer, count, flags, address, address_len); });
    if (rc >= 0) {
      return rc;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_sendto failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kWrite);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_sendto wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

int co_connect(int fd, const struct sockaddr* address, socklen_t address_len, uint32_t timeout_ms) {
  // 非阻塞 connect：
  // 1) connect 返回 EINPROGRESS/EALREADY 时等待可写。
  // 2) 等待结束后必须通过 SO_ERROR 读取真实连接结果。
  ZCOROUTINE_LOG_INFO("co_connect start, fd={}, timeout_ms={}", fd, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, false);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  const int rc = retry_on_eintr([&]() -> ssize_t { return ::connect(fd, address, address_len); });
  if (rc == 0) {
    return 0;
  }

  if (errno != EINPROGRESS && errno != EALREADY) {
    ZCOROUTINE_LOG_WARN("co_connect immediate failure, fd={}, errno={}", fd, errno);
    return -1;
  }

  if (user_nonblocking) {
    return -1;
  }

  IoEvent io_event(fd, IoEventType::kWrite);
  if (!io_event.wait(effective_timeout_ms)) {
    if (errno == 0) {
      errno = ETIMEDOUT;
    }
    ZCOROUTINE_LOG_WARN("co_connect timeout or wait failure, fd={}, timeout_ms={}, errno={}", fd,
                        effective_timeout_ms, errno);
    return -1;
  }

  int socket_error = 0;
  socklen_t socket_error_len = sizeof(socket_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0) {
    ZCOROUTINE_LOG_WARN("co_connect getsockopt failure, fd={}, errno={}", fd, errno);
    return -1;
  }
  if (socket_error != 0) {
    errno = socket_error;
    ZCOROUTINE_LOG_WARN("co_connect socket error, fd={}, socket_error={}", fd, socket_error);
    return -1;
  }

  ZCOROUTINE_LOG_INFO("co_connect success, fd={}", fd);
  return 0;
}

int co_accept(int fd, struct sockaddr* address, socklen_t* address_len, uint32_t timeout_ms) {
  // accept 在监听 socket 上可能反复返回 EAGAIN，需循环等待可读事件。
  ZCOROUTINE_LOG_DEBUG("co_accept start, fd={}, timeout_ms={}", fd, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const int accepted_fd = static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::accept(fd, address, address_len); }));
    if (accepted_fd >= 0) {
      sync_fd_metadata_on_dup(fd, accepted_fd);
      ZCOROUTINE_LOG_INFO("co_accept success, listen_fd={}, accepted_fd={}", fd, accepted_fd);
      return accepted_fd;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_accept failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_accept wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

int co_accept4(int fd,
               struct sockaddr* address,
               socklen_t* address_len,
               int flags,
               uint32_t timeout_ms) {
  ZCOROUTINE_LOG_DEBUG("co_accept4 start, fd={}, flags={}, timeout_ms={}", fd, flags, timeout_ms);
  const bool user_nonblocking = get_user_nonblocking(fd);
  const uint32_t effective_timeout_ms = resolve_timeout_ms(fd, timeout_ms, true);
  FdNonBlockingGuard guard(fd, !user_nonblocking);

  while (true) {
    const int accepted_fd = static_cast<int>(
        retry_on_eintr([&]() -> ssize_t { return ::accept4(fd, address, address_len, flags); }));
    if (accepted_fd >= 0) {
      sync_fd_metadata_on_dup(fd, accepted_fd);
      ZCOROUTINE_LOG_INFO("co_accept4 success, listen_fd={}, accepted_fd={}", fd, accepted_fd);
      return accepted_fd;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ZCOROUTINE_LOG_WARN("co_accept4 failed, fd={}, errno={}", fd, errno);
      return -1;
    }

    if (user_nonblocking) {
      return -1;
    }

    IoEvent io_event(fd, IoEventType::kRead);
    if (!io_event.wait(effective_timeout_ms)) {
      ZCOROUTINE_LOG_DEBUG("co_accept4 wait failed or timeout, fd={}, timeout_ms={}, errno={}", fd,
                           effective_timeout_ms, errno);
      return -1;
    }
  }
}

}  // namespace zcoroutine
