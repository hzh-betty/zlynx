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
      write_timeout_(0), keepalive_timeout_(0),
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
  // 这里的 HookEnabler 只包住 bind/listen 这段准备逻辑。
  // 目的不是让 bind 本身“异步化”，而是确保底层 socket 在本库约定的
  // hook 语义下被初始化成后续 accept/IO 流程期望的状态，避免监听 socket
  // 和连接 socket 的行为模型不一致。
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
  // 一个监听 socket 对应一个 accept 循环。
  // 这个循环运行在 accept_worker_ 的调度线程里，负责不断 accept 新连接，
  // 然后把真正的连接处理转移到 io_worker_ 上。
  // 这样做的目的有两个：
  // 1. accept 和连接收发解耦，避免监听线程被单个连接处理拖慢；
  // 2. 新连接最终在所属 worker 线程上创建协程，能拿到该线程的 TLS /
  //    shared stack / epoll 上下文，避免跨线程恢复协程带来的问题。
  while (!is_stop_.load(std::memory_order_acquire)) {
    Socket::ptr client = sock->accept();
    if (client) {
      // 设置接收超时
      // 这里设置的是 socket 层/ hook 层的读超时基础值。
      // 后面我们还会把 read/write/keepalive 三种 timeout 配置继续传给
      // TcpConnection，让连接对象在“协议级生命周期”里做更细的控制。
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
      // 新连接在进入业务层前，先继承 server 级 timeout 策略：
      // - read_timeout: 建连后等待请求字节到达，以及后续请求读取超时
      // - write_timeout: 响应发送过程中，输出缓冲区长期刷不空则断开
      // - keepalive_timeout: 响应发完后若连接继续保持，则按空闲时间回收
      conn->set_read_timeout(recv_timeout_);
      conn->set_write_timeout(write_timeout_);
      conn->set_keepalive_timeout(keepalive_timeout_);

      // 这样可以确保协程使用正确的线程本地 SharedStack，避免跨线程共享栈问题
      if (io_worker_) {
        auto self = shared_from_this();
        io_worker_->schedule([self, conn = std::move(conn)]() mutable {
          // 在 worker 线程中创建协程，使用 worker 线程的 SharedStack
          // 注意：handle_client() 只是给连接挂上回调、准备协议层逻辑；
          // 真正把连接状态切到 Connected 并注册首次读事件，发生在
          // connect_established() 里。
          auto fiber = zcoroutine::FiberPool::get_instance().get_fiber(
              [self, conn = std::move(conn)]() mutable {
                self->handle_client(conn);
                if (conn->state() == TcpConnection::State::Connecting) {
                  conn->connect_established();
                }
              });
          fiber->resume();
          (void)zcoroutine::FiberPool::get_instance().return_fiber(fiber);
        });
      } else {
        ZNET_LOG_FATAL("TcpServer::start no io_worker_ available");
        throw std::runtime_error("TcpServer::start no io_worker_ available");
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
  // is_stop_ 既是“是否停止”的状态位，也是 start/stop 的幂等保护。
  // compare_exchange 成功说明这次调用真正完成了从 stopped -> running 的切换；
  // 如果失败，表示服务已经启动过，直接返回 true，避免重复启动线程池。
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
  // 与 start() 对称，stop() 也做幂等保护。
  // 只有 running -> stopped 这一次切换真正执行清理逻辑。
  bool expected = false;
  if (!is_stop_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return; // 已经停止
  }

  // 将关闭操作调度到 accept_worker_ 执行，确保线程安全
  auto self = shared_from_this();
  if (accept_worker_) {
    accept_worker_->schedule([this, self]() {
      // 在 accept_worker 线程中执行关闭操作
      // 这里特意把监听 socket 的 close 放回 accept 线程，是为了让
      // 正在 accept() 的循环尽快以 EBADF/中断形式退出，避免不同线程
      // 直接关闭监听 fd 带来的竞态。
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
  << " recv_timeout=" << recv_timeout_
  << " write_timeout=" << write_timeout_
  << " keepalive_timeout=" << keepalive_timeout_ << "]" << std::endl;
  std::string pfx = prefix.empty() ? "    " : prefix;

  zcoroutine::RWMutex::ReadLock lock(socks_mutex_);
  for (auto &i : socks_) {
    ss << pfx << pfx << "fd=" << i->fd()
       << " local=" << i->get_local_address()->to_string() << std::endl;
  }
  return ss.str();
}

} // namespace znet
