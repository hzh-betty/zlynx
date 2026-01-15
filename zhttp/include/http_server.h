#ifndef ZHTTP_HTTP_SERVER_H_
#define ZHTTP_HTTP_SERVER_H_

#include "http_parser.h"
#include "router.h"
#include "tcp_server.h"

#include <memory>
#include <string>

namespace zhttp {

/**
 * @brief HTTP 服务器
 * 继承 TcpServer，处理 HTTP 协议
 */
class HttpServer : public znet::TcpServer {
public:
  using ptr = std::shared_ptr<HttpServer>;

  /**
   * @brief 构造函数
   * @param io_worker IO 调度器
   * @param accept_worker Accept 调度器（可选）
   */
  HttpServer(zcoroutine::IoScheduler::ptr io_worker,
             zcoroutine::IoScheduler::ptr accept_worker = nullptr);

  ~HttpServer() override;

  /**
   * @brief 获取路由器
   * @return 路由器引用
   */
  Router &router() { return router_; }

  /**
   * @brief 设置服务器名称（用于 Server 响应头）
   * @param name 服务器名称
   */
  void set_name(const std::string &name) override;

protected:
  /**
   * @brief 处理新连接
   * @param conn TCP 连接对象
   */
  void handle_client(znet::TcpConnectionPtr conn) override;

private:
  /**
   * @brief 处理 HTTP 消息
   * @param conn TCP 连接对象
   * @param buffer 网络缓冲区
   */
  void on_message(const znet::TcpConnectionPtr &conn, znet::Buffer *buffer);

  /**
   * @brief 处理请求
   * @param conn TCP 连接对象
   * @param request HTTP 请求对象
   */
  void handle_request(const znet::TcpConnectionPtr &conn,
                      const HttpRequest::ptr &request);

private:
  Router router_;
  std::string server_name_ = "zhttp/1.0";
};

} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_H_
