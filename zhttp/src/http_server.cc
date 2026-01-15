#include "http_server.h"

#include "buff.h"
#include "tcp_connection.h"
#include "zhttp_logger.h"

namespace zhttp {

HttpServer::HttpServer(zcoroutine::IoScheduler::ptr io_worker,
                       zcoroutine::IoScheduler::ptr accept_worker)
    : TcpServer(std::move(io_worker), std::move(accept_worker)) {
  set_name("zhttp/1.0");
}

HttpServer::~HttpServer() = default;

void HttpServer::set_name(const std::string &name) {
  TcpServer::set_name(name);
  server_name_ = name;
}

void HttpServer::handle_client(znet::TcpConnectionPtr conn) {
  ZHTTP_LOG_DEBUG("New HTTP connection: {}", conn->name());

  // 设置消息回调
  conn->set_message_callback(
      [this](const znet::TcpConnectionPtr &c, znet::Buffer *buffer) {
        on_message(c, buffer);
      });

  // 设置关闭回调
  conn->set_close_callback([](const znet::TcpConnectionPtr &c) {
    ZHTTP_LOG_DEBUG("HTTP connection closed: {}", c->name());
  });

  // 连接建立
  conn->connect_established();

  // 主动读取数据（协程模式）
  while (conn->connected()) {
    conn->handle_read();

    if (conn->input_buffer()->readable_bytes() > 0) {
      on_message(conn, conn->input_buffer());
    }
  }
}

void HttpServer::on_message(const znet::TcpConnectionPtr &conn,
                            znet::Buffer *buffer) {
  // 创建解析器（每个请求独立）
  HttpParser parser;

  while (buffer->readable_bytes() > 0) {
    ParseResult result = parser.parse(buffer);

    if (result == ParseResult::COMPLETE) {
      // 请求解析完成，处理请求
      handle_request(conn, parser.request());

      // 检查 Keep-Alive
      if (!parser.request()->is_keep_alive()) {
        conn->shutdown();
        return;
      }

      // 重置解析器，准备下一个请求
      parser.reset();
    } else if (result == ParseResult::NEED_MORE) {
      // 需要更多数据，等待下次读取
      return;
    } else if (result == ParseResult::ERROR) {
      // 解析错误
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

void HttpServer::handle_request(const znet::TcpConnectionPtr &conn,
                                const HttpRequest::ptr &request) {
  ZHTTP_LOG_DEBUG("{} {} {}", method_to_string(request->method()),
                  request->path(), version_to_string(request->version()));

  // 创建响应对象
  HttpResponse response;
  response.set_version(request->version());
  response.set_keep_alive(request->is_keep_alive());
  response.header("Server", server_name_);

  // 路由处理
  router_.route(request, response);

  // 发送响应
  std::string response_str = response.serialize();
  conn->send(response_str);

  ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                  status_to_string(response.status_code()));
}

} // namespace zhttp
