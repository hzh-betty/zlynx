#include "tcp_server.h"
#include "fiber_pool.h"
#include "io_scheduler.h"
#include "thread_context.h"
#include "znet_logger.h"
#include <errno.h>
#include <sstream>
#include <string.h>

namespace znet {

TcpServer::TcpServer(zcoroutine::IoScheduler::ptr io_worker,
                     zcoroutine::IoScheduler::ptr accept_worker)
    : io_worker_(io_worker),
      accept_worker_(accept_worker ? accept_worker
                                   : std::make_shared<zcoroutine::IoScheduler>(
                                         1, "TcpServer-Accept", false)),
      recv_timeout_(60 * 1000 * 2), // 默认 2 分钟
      name_("znet/1.0.0"), type_("tcp"), is_stop_(true) {
        zcoroutine::FiberPool::get_instance().init();
      }

TcpServer::~TcpServer() { stop(); }

bool TcpServer::bind(Address::ptr addr) {
  std::vector<Address::ptr> addrs;
  std::vector<Address::ptr> fails;
  addrs.push_back(addr);
  return bind(addrs, fails);
}

bool TcpServer::bind(const std::vector<Address::ptr> &addrs,
                     std::vector<Address::ptr> &fails) {
  // 在该作用域开启hook，用于将监听socket设置为非阻塞
  struct HookEnabler {
    HookEnabler() { zcoroutine::ThreadContext::set_hook_enable(true); }
    ~HookEnabler() { zcoroutine::ThreadContext::set_hook_enable(false); }
  };
  HookEnabler hook_enabler;
  zcoroutine::RWMutex::WriteLock lock(socks_mutex_);
  for (auto &addr : addrs) {
    Socket::ptr sock = Socket::create_tcp(addr);
    if (!sock) {
      ZNET_LOG_ERROR("TcpServer::bind create socket failed for addr={}",
                     addr->to_string());
      fails.push_back(addr);
      continue;
    }

    if (!sock->bind(addr)) {
      ZNET_LOG_ERROR("TcpServer::bind bind fail errno={} errstr={} addr={}",
                     errno, strerror(errno), addr->to_string());
      fails.push_back(addr);
      continue;
    }

    if (!sock->listen()) {
      ZNET_LOG_ERROR("TcpServer::bind listen fail errno={} errstr={} addr={}",
                     errno, strerror(errno), addr->to_string());
      fails.push_back(addr);
      continue;
    }

    socks_.push_back(sock);
  }

  if (!fails.empty()) {
    socks_.clear();
    return false;
  }

  for (auto &i : socks_) {
    ZNET_LOG_INFO("TcpServer type={} name={} bind success: fd={} addr={}",
                  type_, name_, i->fd(), i->get_local_address()->to_string());
  }
  return true;
}

void TcpServer::start_accept(Socket::ptr sock) {
  while (!is_stop_.load(std::memory_order_acquire)) {
    Socket::ptr client = sock->accept();
    if (client) {
      // 设置接收超时
      if (recv_timeout_ > 0) {
        client->set_recv_timeout(recv_timeout_);
      }

      // 创建TcpConnection，转移 Socket 所有权
      auto local_addr = client->get_local_address();
      auto peer_addr = client->get_remote_address();
        std::string conn_name;
        conn_name.reserve(name_.size() + 1 + 64);
        conn_name.append(name_);
        conn_name.push_back('-');
        conn_name.append(peer_addr->to_string());

      TcpConnectionPtr conn = std::make_shared<TcpConnection>(
          std::move(conn_name), std::move(client), local_addr, peer_addr,
          io_worker_.get());

      // 这样可以确保协程使用正确的线程本地 SharedStack，避免跨线程共享栈问题
      if (io_worker_) {
        auto self = shared_from_this();
        io_worker_->schedule([self, conn = std::move(conn)]() mutable {
          // 在 worker 线程中创建协程，使用 worker 线程的 SharedStack
          auto fiber =
              zcoroutine::FiberPool::get_instance().get_fiber([self, conn = std::move(conn)]() mutable {
                // 处理连接
                self->handle_client(conn);
                conn->connect_established();
              });
          fiber->resume();
          (void)zcoroutine::FiberPool::get_instance().return_fiber(fiber);
        });
      } else {
        handle_client(conn);
        conn->connect_established();
      }
    } else {
      if (is_stop_.load(std::memory_order_acquire)) {
        break;
      }

      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        continue;
      }
      if (err == EINTR || err == EBADF) {
        // EINTR: 被信号中断；EBADF: 监听 socket 已关闭
        break;
      }
      ZNET_LOG_ERROR("TcpServer::start_accept accept errno={} errstr={}", err,
                     strerror(err));
    }
  }
}

bool TcpServer::start() {
  bool expected = true;
  if (!is_stop_.compare_exchange_strong(expected, false,
                                        std::memory_order_acq_rel)) {
    return true; // 已经启动
  }

  // 启动 io_worker_
  if (io_worker_) {
    io_worker_->start();
  }

  // 启动 accept_worker_
  if (accept_worker_) {
    accept_worker_->start();
  }

  zcoroutine::RWMutex::ReadLock lock(socks_mutex_);
  for (auto &sock : socks_) {
    if (accept_worker_) {
      // 使用协程池创建协程用于接受连接
      auto self = shared_from_this();
      auto fiber = zcoroutine::FiberPool::get_instance().get_fiber(
          [self, sock]() { self->start_accept(sock); });
      accept_worker_->schedule(std::move(fiber));
    } else {
      ZNET_LOG_ERROR("TcpServer::start no scheduler available");
      return false;
    }
  }
  return true;
}

void TcpServer::stop() {
  bool expected = false;
  if (!is_stop_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return; // 已经停止
  }

  //将关闭操作调度到 accept_worker_ 执行，确保线程安全
  auto self = shared_from_this();
  if (accept_worker_) {
    accept_worker_->schedule([this, self]() {
      // 在 accept_worker 线程中执行关闭操作
      {
        zcoroutine::RWMutex::WriteLock lock(socks_mutex_);
        for (auto &sock : socks_) {
          sock->close();
        }
        socks_.clear();
      }
    });
  } else {
    // 没有 accept_worker，直接关闭
    zcoroutine::RWMutex::WriteLock lock(socks_mutex_);
    for (auto &sock : socks_) {
      sock->close();
    }
    socks_.clear();
  }

  // 停止 accept_worker_（同步等待完成）
  if (accept_worker_) {
    accept_worker_->stop();
  }

  // 停止 io_worker_（同步等待完成）
  if (io_worker_) {
    io_worker_->stop();
  }
}

void TcpServer::handle_client(TcpConnectionPtr conn) {
  ZNET_LOG_INFO("TcpServer::handle_client connection [{}] fd={} remote={}",
                conn->name(), conn->socket()->fd(),
                conn->peer_address()->to_string());

  // 默认实现：什么都不做
  // 子类应该重写这个方法来处理客户端连接
}

std::string TcpServer::to_string(const std::string &prefix) {
  std::stringstream ss;
  ss << prefix << "[type=" << type_ << " name=" << name_
     << " recv_timeout=" << recv_timeout_ << "]" << std::endl;
  std::string pfx = prefix.empty() ? "    " : prefix;

  zcoroutine::RWMutex::ReadLock lock(socks_mutex_);
  for (auto &i : socks_) {
    ss << pfx << pfx << "fd=" << i->fd()
       << " local=" << i->get_local_address()->to_string() << std::endl;
  }
  return ss.str();
}

} // namespace znet
