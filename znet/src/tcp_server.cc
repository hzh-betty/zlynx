#include "znet/tcp_server.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

#include "znet/socket.h"
#include "zcoroutine/sched.h"
#include "znet/session.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {

// 默认流工厂：读写都使用 BufferStream，覆盖大多数应用场景。
std::pair<Stream::ptr, Stream::ptr> make_default_stream_pair() {
  return {std::make_shared<BufferStream>(), std::make_shared<BufferStream>()};
}

// 读循环中可重试的短暂错误。
bool is_retryable_read_errno(int err) {
  return err == EINTR || err == EAGAIN || err == EWOULDBLOCK ||
         err == ETIMEDOUT;
}

// 代表连接已不可继续使用的错误。
bool is_peer_disconnect_errno(int err) {
  return err == ECONNRESET || err == ENOTCONN || err == EPIPE;
}

}  // namespace

// 规范化流工厂返回值，确保读写流至少都有可用实例。
std::pair<Stream::ptr, Stream::ptr> TcpServer::create_stream_pair() const {
  if (!stream_factory_) {
    return make_default_stream_pair();
  }

  auto streams = stream_factory_();
  if (!streams.first && !streams.second) {
    return make_default_stream_pair();
  }

  if (!streams.first) {
    streams.first = streams.second;
  }
  if (!streams.second) {
    streams.second = streams.first;
  }
  return streams;
}

// 默认连接高水位阈值 64MB，线程数由外部 set_thread_count 控制。
TcpServer::TcpServer(Address::ptr listen_address, int backlog)
    : acceptor_(std::make_shared<Acceptor>(std::move(listen_address), backlog)),
      high_water_mark_(64 * 1024 * 1024),thread_count_(0),
      connection_registry_sched_(nullptr) {}

// 启动流程：初始化协程运行时 -> 选择连接表归属调度器 -> 启动 acceptor。
bool TcpServer::do_start() {
  if (!acceptor_) {
    ZNET_LOG_ERROR("TcpServer::do_start failed because acceptor is null");
    return false;
  }

  zcoroutine::init(thread_count_); // 初始化协程调度器线程池
  ZNET_LOG_INFO("TcpServer::do_start initialized zcoroutine runtime: thread_count={}",
                thread_count_);

  connection_registry_sched_ = zcoroutine::main_sched();
  if (!connection_registry_sched_) {
    connection_registry_sched_ = zcoroutine::next_sched();
  }
  if (!connection_registry_sched_) {
    errno = EOPNOTSUPP;
    ZNET_LOG_ERROR("TcpServer::start requires zcoroutine::init before start");
    return false;
  }

  ZNET_LOG_INFO("TcpServer::do_start using connection registry scheduler: id={}",
                connection_registry_sched_->id());

  acceptor_->set_accept_callback(
      [this](Socket::ptr client) { handle_connection(std::move(client)); });

  const bool ok = acceptor_->start();
  if (ok) {
    ZNET_LOG_INFO("TcpServer::do_start acceptor started successfully");
  } else {
    ZNET_LOG_ERROR("TcpServer::do_start failed to start acceptor");
  }
  return ok;
}

// 停机流程：停止接入 -> 关闭并清空连接表（在 registry 调度器上串行执行）。
void TcpServer::do_stop() {
  ZNET_LOG_INFO("TcpServer::do_stop begin");
  if (acceptor_) {
    acceptor_->stop();
  }

  auto close_all = [this]() {
    ConnectionMap connections_snapshot;
    // 先 swap 再遍历，缩短持有共享映射的时间窗口。
    connections_snapshot.swap(connections_);
    for (auto& item : connections_snapshot) {
      if (item.second) {
        item.second->close();
      }
    }
    ZNET_LOG_INFO("TcpServer::do_stop closed all connections: count={}",
                  connections_snapshot.size());
  };

  if (!connection_registry_sched_) {
    close_all();
    return;
  }

  if (zcoroutine::in_coroutine() &&
      zcoroutine::sched_id() == connection_registry_sched_->id()) {
    close_all();
    return;
  }

  std::atomic<bool> done{false};
  connection_registry_sched_->go([&done, close_all]() mutable {
    close_all();
    done.store(true, std::memory_order_release);
  });

  // 等待异步回收完成，确保 stop() 返回时连接已全部关闭。
  while (!done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ZNET_LOG_INFO("TcpServer::do_stop completed");
}

// 新连接处理：构造 TcpConnection，建立读循环并驱动业务回调。
void TcpServer::handle_connection(Socket::ptr client) {
  if (!client) {
    ZNET_LOG_WARN("TcpServer::handle_connection ignored null client socket");
    return;
  }

  zcoroutine::Scheduler* scheduler = zcoroutine::next_sched();
  if (!scheduler) {
    scheduler = connection_registry_sched_;
  }

  ZNET_LOG_DEBUG("TcpServer::handle_connection dispatch: client_fd={}, sched_id={}",
                 client->fd(), scheduler ? scheduler->id() : -1);

  std::shared_ptr<TcpServer> self = shared_from_this();
  auto run_connection = [self, scheduler, client = std::move(client)]() mutable {
    ZNET_LOG_INFO("TcpServer::handle_connection begin: client_fd={}", client->fd());
    TcpConnection::ptr connection = std::make_shared<TcpConnection>(client);
    connection->bind_to_loop(scheduler);

    // 每个连接独立创建流对象，避免连接间缓冲状态串扰。
    auto streams = self->create_stream_pair();
    connection->set_streams(streams.first, streams.second);

    if (self->on_write_complete_callback_) {
      connection->set_write_complete_callback(self->on_write_complete_callback_);
    }
    if (self->on_high_water_mark_callback_) {
      connection->set_high_water_mark_callback(self->on_high_water_mark_callback_,
                                               self->high_water_mark_);
    }

    self->register_connection(connection);

    if (self->on_connection_callback_) {
      self->on_connection_callback_(connection);
    }

    streams.first->on_open();
    if (streams.second != streams.first) {
      streams.second->on_open();
    }

    while (self->is_running() && connection->connected()) {
      const ssize_t n = connection->read();
      if (n > 0) {
        // 数据就绪后由上层协议在 read_stream 中消费。
        if (self->on_message_callback_) {
          self->on_message_callback_(connection, streams.first);
        }
        continue;
      }

      if (n == 0) {
        ZNET_LOG_INFO("TcpServer::handle_connection peer closed: fd={}",
                      connection->fd());
        break;
      }

      const int read_err = errno;
      if (is_retryable_read_errno(read_err)) {
        // 短暂错误让出执行权，后续继续读循环。
        zcoroutine::yield();
        continue;
      }

      // 除非是对端断开，否则记录警告日志并继续保持连接，等待下一次可读事件。
      if (is_peer_disconnect_errno(read_err) || read_err == EBADF) {
        ZNET_LOG_INFO("TcpServer::handle_connection disconnected by peer/error: "
                      "fd={}, errno={}",
                      connection->fd(), read_err);
        break;
      }

      ZNET_LOG_WARN(
          "TcpServer::handle_connection keep alive on read error: fd={}, "
          "errno={}",
          connection->fd(), read_err);
      zcoroutine::yield();
    }

    streams.first->on_close();
    if (streams.second != streams.first) {
      streams.second->on_close();
    }
    if (self->on_close_callback_) {
      self->on_close_callback_(connection);
    }
    connection->close();
    self->remove_connection(connection->fd());
    ZNET_LOG_INFO("TcpServer::handle_connection end: fd={}", connection->fd());
  };

  if (!scheduler) {
    // 无可用调度器时在当前上下文直接执行，保证连接不丢失。
    run_connection();
    return;
  }

  scheduler->go(std::move(run_connection));
}

// 在连接表归属调度器上登记连接，保证映射并发安全。
void TcpServer::register_connection(const TcpConnection::ptr& connection) {
  if (!connection) {
    ZNET_LOG_WARN("TcpServer::register_connection ignored null connection");
    return;
  }

  const int fd = connection->fd();
  auto task = [self = shared_from_this(), fd, connection]() {
    self->connections_[fd] = connection;
    ZNET_LOG_DEBUG("TcpServer::register_connection success: fd={}, total={}", fd,
                   self->connections_.size());
  };

  if (!connection_registry_sched_) {
    task();
    return;
  }

  if (zcoroutine::in_coroutine() &&
      zcoroutine::sched_id() == connection_registry_sched_->id()) {
    task();
    return;
  }

  connection_registry_sched_->go(std::move(task));
}

// 在连接表归属调度器上移除连接，避免跨调度器并发修改。
void TcpServer::remove_connection(int fd) {
  auto task = [self = shared_from_this(), fd]() {
    self->connections_.erase(fd);
    ZNET_LOG_DEBUG("TcpServer::remove_connection success: fd={}, total={}", fd,
                   self->connections_.size());
  };

  if (!connection_registry_sched_) {
    task();
    return;
  }

  if (zcoroutine::in_coroutine() &&
      zcoroutine::sched_id() == connection_registry_sched_->id()) {
    task();
    return;
  }

  connection_registry_sched_->go(std::move(task));
}

}  // namespace znet
