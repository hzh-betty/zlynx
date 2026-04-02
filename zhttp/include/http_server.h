#ifndef ZHTTP_HTTP_SERVER_H_
#define ZHTTP_HTTP_SERVER_H_

#include "http_parser.h"
#include "router.h"
#include "znet/address.h"
#include "znet/tcp_connection.h"
#include "znet/tcp_server.h"

#include <cstdint>
#include <memory>
#include <string>

#include <sys/socket.h>

namespace zhttp {

/**
 * @brief HTTP 服务器
 * @details
 * HttpServer 复用底层 TcpServer 的连接管理能力，在消息回调里完成 HTTP 解析、
 * 路由分发和响应回写。它关注的是“如何把一个 TCP 字节流变成 HTTP 请求并交给业务”。
 */
class HttpServer {
public:
  using ptr = std::shared_ptr<HttpServer>;

  explicit HttpServer(znet::Address::ptr listen_address,
                      int backlog = SOMAXCONN);

  virtual ~HttpServer();

  /**
   * @brief 获取路由器
    * @return 内部路由器引用，可用于注册路由和中间件
   */
  Router &router() { return router_; }

  void set_name(const std::string &name);

  const std::string &name() const { return server_name_; }

  void set_thread_count(size_t thread_count);

  void set_recv_timeout(uint64_t timeout_ms);

  void set_write_timeout(uint64_t timeout_ms);

  void set_keepalive_timeout(uint64_t timeout_ms);

  /**
   * @brief 启用 HTTPS（TLS）
   * @param cert_file 证书文件路径
   * @param key_file 私钥文件路径
   * @return 是否初始化成功
   */
  bool set_ssl_certificate(const std::string &cert_file,
                           const std::string &key_file);

  bool start();

  void stop();

  bool is_running() const;

protected:
  znet::TcpServer::ptr tcp_server() const { return tcp_server_; }

  uint32_t write_timeout() const {
    return tcp_server_ ? tcp_server_->write_timeout() : 0;
  }

  virtual void on_connection(const znet::TcpConnection::ptr &conn);

  virtual void on_message(const znet::TcpConnection::ptr &conn,
                          znet::Buffer &buffer);

  virtual void on_close(const znet::TcpConnection::ptr &conn);

  virtual bool handle_request(const znet::TcpConnection::ptr &conn,
                              const HttpRequest::ptr &request);

  HttpParser *ensure_parser(const znet::TcpConnection::ptr &conn);

private:
  znet::TcpServer::ptr tcp_server_;

  // 负责路径匹配、中间件执行和业务处理器调度。
  Router router_;

  // Server 响应头默认值。
  std::string server_name_ = "zhttp/1.0";
};

} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_H_
