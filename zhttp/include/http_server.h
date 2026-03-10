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
 * @details
 * HttpServer 复用底层 TcpServer 的连接管理能力，在消息回调里完成 HTTP 解析、
 * 路由分发和响应回写。它关注的是“如何把一个 TCP 字节流变成 HTTP 请求并交给业务”。
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
    * @return 内部路由器引用，可用于注册路由和中间件
   */
  Router &router() { return router_; }

  /**
   * @brief 设置服务器名称（用于 Server 响应头）
    * @param name 服务器名称
    * @details
    * 该名称既会同步到底层 TcpServer，也会用于后续响应里的 Server 头部。
   */
  void set_name(const std::string &name) override;

protected:
  /**
   * @brief 处理新连接
   * @param conn TCP 连接对象
    * @details
    * 这里主要给连接绑定消息回调和关闭回调，真正的 HTTP 解析在 on_message() 中完成。
   */
  void handle_client(znet::TcpConnectionPtr conn) override;

private:
  /**
   * @brief 处理 HTTP 消息
   * @param conn TCP 连接对象
   * @param buffer 网络缓冲区
    * @details
    * 一个连接上可能连续收到多个请求，也可能一个请求分多次到达，因此该函数内部
    * 会循环驱动解析器，直到数据耗尽、请求不完整或出现错误。
   */
  void on_message(const znet::TcpConnectionPtr &conn, znet::Buffer *buffer);

  /**
   * @brief 处理请求
   * @param conn TCP 连接对象
   * @param request HTTP 请求对象
   * @details
   * 该阶段会补全请求上下文、构造响应对象、调用路由器，然后把序列化后的响应发送回客户端。
   */
  bool handle_request(const znet::TcpConnectionPtr &conn,
                      const HttpRequest::ptr &request);

private:
  // 负责路径匹配、中间件执行和业务处理器调度。
  Router router_;

  // Server 响应头默认值。
  std::string server_name_ = "zhttp/1.0";
};

} // namespace zhttp

#endif // ZHTTP_HTTP_SERVER_H_
