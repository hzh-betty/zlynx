#include "znet/tcp_client.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <string>

#include "zcoroutine/hook.h"
#include "zcoroutine/sched.h"

#include "znet/ssl.h"

namespace znet {
namespace {

uint32_t to_timeout_ms(int ms) {
  return ms < 0 ? zcoroutine::kInfiniteTimeoutMs : static_cast<uint32_t>(ms);
}

std::string errno_message() {
  if (errno == 0) {
    return "success";
  }
  return std::strerror(errno);
}

class ClientImpl {
 public:
  ClientImpl(const char* ip, int port, bool use_ssl)
      : ip_(ip != nullptr && *ip != '\0' ? ip : "127.0.0.1"),
        port_(port),
        use_ssl_(use_ssl),
        fd_(-1),
        connected_(false),
        ssl_ctx_(nullptr),
        ssl_(nullptr) {}

  ClientImpl(const ClientImpl& c)
      : ip_(c.ip_),
        port_(c.port_),
        use_ssl_(c.use_ssl_),
        fd_(-1),
        connected_(false),
        ssl_ctx_(nullptr),
        ssl_(nullptr) {}

  ~ClientImpl() { this->disconnect(); }

  int recv(void* buf, int n, int ms) {
    if (!connected_) {
      errno = ENOTCONN;
      last_error_ = errno_message();
      return -1;
    }

    if (!use_ssl_) {
      const ssize_t r = zcoroutine::co_recv(fd_, buf, static_cast<size_t>(n), 0, to_timeout_ms(ms));
      if (r < 0) {
        last_error_ = errno_message();
      }
      return static_cast<int>(r);
    }

    const int r = ssl::recv(ssl_, buf, n, ms);
    if (r < 0) {
      last_error_ = ssl::strerror(ssl_);
    }
    return r;
  }

  int recvn(void* buf, int n, int ms) {
    if (!connected_) {
      errno = ENOTCONN;
      last_error_ = errno_message();
      return -1;
    }

    if (!use_ssl_) {
      const ssize_t r = zcoroutine::co_recvn(fd_, buf, static_cast<size_t>(n), 0, to_timeout_ms(ms));
      if (r < 0) {
        last_error_ = errno_message();
      }
      return static_cast<int>(r);
    }

    const int r = ssl::recvn(ssl_, buf, n, ms);
    if (r < 0) {
      last_error_ = ssl::strerror(ssl_);
    }
    return r;
  }

  int send(const void* buf, int n, int ms) {
    if (!connected_) {
      errno = ENOTCONN;
      last_error_ = errno_message();
      return -1;
    }

    if (!use_ssl_) {
      const ssize_t r = zcoroutine::co_send(fd_, buf, static_cast<size_t>(n), 0, to_timeout_ms(ms));
      if (r <= 0) {
        last_error_ = errno_message();
      }
      return static_cast<int>(r);
    }

    const int r = ssl::send(ssl_, buf, n, ms);
    if (r <= 0) {
      last_error_ = ssl::strerror(ssl_);
    }
    return r;
  }

  bool bind(const char* ip, int port) {
    if (connected_) {
      errno = EISCONN;
      last_error_ = errno_message();
      return false;
    }

    if (ip == nullptr || *ip == '\0') {
      errno = EINVAL;
      last_error_ = errno_message();
      return false;
    }

    const std::string service_port = std::to_string(port_);
    struct addrinfo* serv = nullptr;
    int r = getaddrinfo(ip_.c_str(), service_port.c_str(), nullptr, &serv);
    if (r != 0) {
      last_error_ = gai_strerror(r);
      return false;
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> serv_guard(serv, freeaddrinfo);
    if (fd_ == -1) {
      fd_ = zcoroutine::co_tcp_socket(serv->ai_family);
      if (fd_ == -1) {
        last_error_ = errno_message();
        return false;
      }
    }

    const std::string bind_port = std::to_string(port);
    struct addrinfo* cli = nullptr;
    r = getaddrinfo(ip, bind_port.c_str(), nullptr, &cli);
    if (r != 0) {
      last_error_ = gai_strerror(r);
      return false;
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> cli_guard(cli, freeaddrinfo);
    if (zcoroutine::co_bind(fd_, cli->ai_addr, static_cast<socklen_t>(cli->ai_addrlen)) != 0) {
      last_error_ = errno_message();
      return false;
    }

    return true;
  }

  bool connect(int ms) {
    if (connected_) {
      return true;
    }

    if (!zcoroutine::in_coroutine()) {
      errno = EPERM;
      last_error_ = "connect must be called in coroutine context";
      return false;
    }

    struct addrinfo* info = nullptr;
    const std::string port_text = std::to_string(port_);
    int r = getaddrinfo(ip_.c_str(), port_text.c_str(), nullptr, &info);
    if (r != 0) {
      last_error_ = gai_strerror(r);
      return false;
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> info_guard(info, freeaddrinfo);

    if (fd_ == -1) {
      fd_ = zcoroutine::co_tcp_socket(info->ai_family);
      if (fd_ == -1) {
        last_error_ = errno_message();
        return false;
      }
    }

    r = zcoroutine::co_connect(fd_, info->ai_addr, static_cast<socklen_t>(info->ai_addrlen), to_timeout_ms(ms));
    if (r != 0) {
      last_error_ = errno_message();
      this->disconnect();
      return false;
    }

    zcoroutine::co_set_tcp_nodelay(fd_);
    if (use_ssl_) {
      ssl_ctx_ = ssl::new_client_ctx();
      if (ssl_ctx_ == nullptr) {
        last_error_ = ssl::strerror();
        this->disconnect();
        return false;
      }

      ssl_ = ssl::new_ssl(ssl_ctx_);
      if (ssl_ == nullptr) {
        last_error_ = ssl::strerror();
        this->disconnect();
        return false;
      }

      if (ssl::set_fd(ssl_, fd_) != 1) {
        last_error_ = ssl::strerror(ssl_);
        this->disconnect();
        return false;
      }

      if (ssl::connect(ssl_, ms) != 1) {
        last_error_ = ssl::strerror(ssl_);
        this->disconnect();
        return false;
      }
    }

    connected_ = true;
    last_error_.clear();
    return true;
  }

  void disconnect() {
    if (fd_ != -1) {
      if (use_ssl_) {
        if (ssl_ != nullptr) {
          ssl::free_ssl(ssl_);
          ssl_ = nullptr;
        }
        if (ssl_ctx_ != nullptr) {
          ssl::free_ctx(ssl_ctx_);
          ssl_ctx_ = nullptr;
        }
      }

      zcoroutine::co_close(fd_);
      fd_ = -1;
    }

    connected_ = false;
  }

  bool connected() const noexcept {
    return connected_;
  }

  const char* strerror() const {
    if (!last_error_.empty()) {
      return last_error_.c_str();
    }

    if (use_ssl_) {
      return ssl::strerror(ssl_);
    }

    if (errno != 0) {
      return std::strerror(errno);
    }

    return "success";
  }

  int socket() const noexcept {
    return fd_;
  }

 private:
  std::string ip_;
  int port_;
  bool use_ssl_;
  int fd_;
  bool connected_;
  ssl::C* ssl_ctx_;
  ssl::S* ssl_;
  mutable std::string last_error_;
};

}  // namespace

TcpClient::TcpClient(const char* ip, int port, bool use_ssl)
    : impl_(new ClientImpl(ip, port, use_ssl)) {}

TcpClient::TcpClient(const TcpClient& c)
    : impl_(new ClientImpl(*static_cast<ClientImpl*>(c.impl_))) {}

TcpClient::~TcpClient() {
  delete static_cast<ClientImpl*>(impl_);
  impl_ = nullptr;
}

int TcpClient::recv(void* buf, int n, int ms) {
  return static_cast<ClientImpl*>(impl_)->recv(buf, n, ms);
}

int TcpClient::recvn(void* buf, int n, int ms) {
  return static_cast<ClientImpl*>(impl_)->recvn(buf, n, ms);
}

int TcpClient::send(const void* buf, int n, int ms) {
  return static_cast<ClientImpl*>(impl_)->send(buf, n, ms);
}

bool TcpClient::bind(const char* ip, int port) {
  return static_cast<ClientImpl*>(impl_)->bind(ip, port);
}

bool TcpClient::connected() const noexcept {
  return static_cast<const ClientImpl*>(impl_)->connected();
}

bool TcpClient::connect(int ms) {
  return static_cast<ClientImpl*>(impl_)->connect(ms);
}

void TcpClient::disconnect() {
  static_cast<ClientImpl*>(impl_)->disconnect();
}

const char* TcpClient::strerror() const {
  return static_cast<const ClientImpl*>(impl_)->strerror();
}

int TcpClient::socket() const noexcept {
  return static_cast<const ClientImpl*>(impl_)->socket();
}

}  // namespace znet
