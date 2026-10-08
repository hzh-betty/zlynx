#pragma once
#include "zhttp/middleware/middleware.h"
#include "zhttp/request_limits.h"
#include "zhttp/router/router.h"
#include "znet/address.h"
#include <cstdint>
#include <exception>
#include <memory>
#include <sys/socket.h>
namespace znet {
class TcpServer;
}
namespace zhttp {
using ExceptionHandler = std::function<void(HttpContext &, std::exception_ptr)>;
class HttpServer {
  public:
    using ptr = std::shared_ptr<HttpServer>;
    explicit HttpServer(znet::Address::ptr address, int backlog = SOMAXCONN);
    virtual ~HttpServer();
    Router &router();
    void use(mid::Middleware::ptr middleware);
    void use(const std::string &path, mid::Middleware::ptr middleware);
    void use_group(const std::string &prefix, mid::Middleware::ptr middleware);
    void set_not_found_handler(HttpHandler handler);
    void set_exception_handler(ExceptionHandler handler);
    void set_name(const std::string &name);
    const std::string &name() const;
    void set_thread_count(size_t count);
    void set_recv_timeout(uint64_t timeout);
    void set_write_timeout(uint64_t timeout);
    void set_keepalive_timeout(uint64_t timeout);
    void set_request_limits(const RequestLimits &limits);
    void set_request_timeout(uint32_t timeout);
    bool set_ssl_certificate(const std::string &cert, const std::string &key);
    bool start();
    void stop();
    bool is_running() const;
    bool handle(HttpContext &context);

  protected:
    std::shared_ptr<znet::TcpServer> tcp_server() const { return tcp_server_; }

  private:
    void check_mutable() const;
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<znet::TcpServer> tcp_server_;
};
} // namespace zhttp
