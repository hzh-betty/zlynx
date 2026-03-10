#include "http_server.h"

#include "buff.h"
#include "tcp_connection.h"
#include "zhttp_logger.h"

namespace zhttp {

HttpServer::HttpServer(zcoroutine::IoScheduler::ptr io_worker,
                       zcoroutine::IoScheduler::ptr accept_worker)
    : TcpServer(std::move(io_worker), std::move(accept_worker)) {
  // 默认把 Server 响应头和底层服务名都设置成统一值。
  set_name("zhttp/1.0");
}

HttpServer::~HttpServer() = default;

void HttpServer::set_name(const std::string &name) {
  TcpServer::set_name(name);
  server_name_ = name;
}

void HttpServer::handle_client(znet::TcpConnectionPtr conn) {
  ZHTTP_LOG_DEBUG("New HTTP connection: {}", conn->name());

  // 消息回调负责把收到的字节流交给 HTTP 解析流程。
  conn->set_message_callback(
      [this](const znet::TcpConnectionPtr &c, znet::Buffer *buffer) {
        on_message(c, buffer);
      });

  // 关闭回调这里只做日志记录，资源释放交给连接对象自身生命周期管理。
  conn->set_close_callback([](const znet::TcpConnectionPtr &c) {
    ZHTTP_LOG_DEBUG("HTTP connection closed: {}", c->name());
  });
}

void HttpServer::on_message(const znet::TcpConnectionPtr &conn,
                            znet::Buffer *buffer) {
  /**
   * 这里的循环有两个目的：
   * 1. 处理一个缓冲区里可能连续到达的多个请求。
   * 2. 在请求不完整时尽早返回，等下次网络数据到来后继续。
   */

  // 当前实现为每次消息回调创建一个解析器，并在本次缓冲区上顺序解析请求。
  HttpParser parser;

  while (buffer->readable_bytes() > 0) {
    ParseResult result = parser.parse(buffer);

    if (result == ParseResult::COMPLETE) {
      // 一条完整请求已经拿到，可以交给业务层处理。
      const bool keep_alive = handle_request(conn, parser.request());

      // 客户端如果不希望保持连接，就在当前响应发完后主动关闭。
      if (!keep_alive) {
        conn->finish_response(false);
        conn->shutdown();
        return;
      }

      if (buffer->readable_bytes() == 0) {
        conn->finish_response(true);
      }

      // 连接仍然保持时，继续尝试解析缓冲区里后续可能已经到达的请求。
      parser.reset();
    } else if (result == ParseResult::NEED_MORE) {
      // 半包场景，等待下一次 on_message 再继续解析。
      return;
    } else if (result == ParseResult::ERROR) {
      // 请求报文不合法时直接返回 400，并关闭连接，避免后续状态混乱。
      ZHTTP_LOG_WARN("HTTP parse error: {}", parser.error());
      HttpResponse response;
      response.status(HttpStatus::BAD_REQUEST)
          .content_type("text/plain")
          .body("Bad Request: " + parser.error());
      response.set_keep_alive(false);
      conn->send(response.serialize());
      conn->shutdown();
      return;
    }
  }
}

bool HttpServer::handle_request(const znet::TcpConnectionPtr &conn,
                                const HttpRequest::ptr &request) {
  ZHTTP_LOG_DEBUG("{} {} {}", method_to_string(request->method()),
                  request->path(), version_to_string(request->version()));

  // 把对端地址补进请求对象，便于日志、鉴权、限流等上层逻辑直接读取。
  if (conn && conn->peer_address()) {
    request->set_remote_addr(conn->peer_address()->to_string());
  }

  // 响应对象的协议版本和 Keep-Alive 策略通常跟随请求。
  HttpResponse response;
  response.set_version(request->version());
  response.set_keep_alive(request->is_keep_alive());
  response.header("Server", server_name_);

  // 路由器内部会完成匹配、中间件执行和业务处理器调用。
  router_.route(request, response);

  // 最终把响应序列化成标准 HTTP 报文并发回客户端。
  std::string response_str = response.serialize();
  conn->send(response_str);

  ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                  status_to_string(response.status_code()));

  return response.is_keep_alive();
}

} // namespace zhttp
