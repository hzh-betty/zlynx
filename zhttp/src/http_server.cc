#include "http_server.h"

#include "zhttp_logger.h"

#include <limits>
#include <memory>
#include <utility>

namespace zhttp {

namespace {

uint32_t clamp_timeout_to_u32(const uint64_t timeout_ms) {
  const uint64_t max_timeout =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1;
  if (timeout_ms >= max_timeout) {
    return static_cast<uint32_t>(max_timeout);
  }
  return static_cast<uint32_t>(timeout_ms);
}

} // namespace

HttpServer::HttpServer(znet::Address::ptr listen_address, int backlog)
    : tcp_server_(
          std::make_shared<znet::TcpServer>(std::move(listen_address), backlog)) {
  tcp_server_->set_on_connection(
      [this](const znet::TcpConnection::ptr &conn) { on_connection(conn); });
  tcp_server_->set_on_message([this](const znet::TcpConnection::ptr &conn,
                                     znet::Buffer &buffer) {
    on_message(conn, buffer);
  });
  tcp_server_->set_on_close(
      [this](const znet::TcpConnection::ptr &conn) { on_close(conn); });

  // 默认把 Server 响应头和底层服务名都设置成统一值。
  set_name("zhttp/1.0");
}

HttpServer::~HttpServer() {
  if (tcp_server_) {
    tcp_server_->stop();
  }
}

void HttpServer::set_name(const std::string &name) {
  server_name_ = name;
}

void HttpServer::set_thread_count(size_t thread_count) {
  if (!tcp_server_) {
    return;
  }

  const size_t max_count = static_cast<size_t>(std::numeric_limits<int>::max());
  if (thread_count > max_count) {
    thread_count = max_count;
  }
  tcp_server_->set_thread_count(static_cast<int>(thread_count));
}

void HttpServer::set_recv_timeout(uint64_t timeout_ms) {
  if (!tcp_server_) {
    return;
  }
  tcp_server_->set_read_timeout(clamp_timeout_to_u32(timeout_ms));
}

void HttpServer::set_write_timeout(uint64_t timeout_ms) {
  if (!tcp_server_) {
    return;
  }
  tcp_server_->set_write_timeout(clamp_timeout_to_u32(timeout_ms));
}

void HttpServer::set_keepalive_timeout(uint64_t timeout_ms) {
  if (!tcp_server_) {
    return;
  }
  tcp_server_->set_keepalive_timeout(timeout_ms);
}

bool HttpServer::set_ssl_certificate(const std::string &cert_file,
                                     const std::string &key_file) {
  if (!tcp_server_) {
    return false;
  }

  return tcp_server_->enable_tls(cert_file, key_file);
}

bool HttpServer::start() {
  if (!tcp_server_) {
    return false;
  }
  return tcp_server_->start();
}

void HttpServer::stop() {
  if (tcp_server_) {
    tcp_server_->stop();
  }
}

bool HttpServer::is_running() const {
  return tcp_server_ && tcp_server_->is_running();
}

HttpParser *HttpServer::ensure_parser(const znet::TcpConnection::ptr &conn) {
  if (!conn) {
    return nullptr;
  }

  auto *parser = static_cast<HttpParser *>(conn->context());
  if (!parser) {
    parser = new HttpParser();
    conn->set_context(parser);
  }
  return parser;
}

void HttpServer::on_connection(const znet::TcpConnection::ptr &conn) {
  if (!conn) {
    return;
  }

  ensure_parser(conn);
  ZHTTP_LOG_DEBUG("New connection: fd={}", conn->fd());
}

void HttpServer::on_close(const znet::TcpConnection::ptr &conn) {
  if (!conn) {
    return;
  }

  auto *parser = static_cast<HttpParser *>(conn->context());
  delete parser;
  conn->set_context(nullptr);

  ZHTTP_LOG_DEBUG("Connection closed: fd={}", conn->fd());
}

void HttpServer::on_message(const znet::TcpConnection::ptr &conn,
                            znet::Buffer &buffer) {
  if (!conn) {
    return;
  }

  /**
   * 这里的循环有两个目的：
   * 1. 处理一个缓冲区里可能连续到达的多个请求。
   * 2. 在请求不完整时尽早返回，等下次网络数据到来后继续。
   */

  // 解析器挂在连接上下文里，拆包时可以跨多次 on_message 持续推进状态机。
  auto *parser = ensure_parser(conn);
  if (!parser) {
    return;
  }

  while (buffer.readable_bytes() > 0) {
    ParseResult result = parser->parse(&buffer);

    if (result == ParseResult::COMPLETE) {
      // 一条完整请求已经拿到，可以交给业务层处理。
      const bool keep_alive = handle_request(conn, parser->request());

      // 客户端如果不希望保持连接，就在当前响应发完后主动关闭。
      if (!keep_alive) {
        conn->shutdown();
        return;
      }

      // 连接仍然保持时，继续尝试解析缓冲区里后续可能已经到达的请求。
      parser->reset();
    } else if (result == ParseResult::NEED_MORE) {
      // 半包场景，等待下一次 on_message 再继续解析。
      return;
    } else if (result == ParseResult::ERROR) {
      // 请求报文不合法时直接返回 400，并关闭连接，避免后续状态混乱。
      ZHTTP_LOG_WARN("HTTP parse error: {}", parser->error());
      HttpResponse response;
      response.status(HttpStatus::BAD_REQUEST)
          .content_type("text/plain")
          .body("Bad Request: " + parser->error());
      response.set_keep_alive(false);
      const std::string payload = response.serialize();
      if (conn->send(payload.data(), payload.size()) < 0) {
        ZHTTP_LOG_WARN("Send HTTP 400 failed: fd={}", conn->fd());
      }
      conn->shutdown();
      return;
    }
  }
}

bool HttpServer::handle_request(const znet::TcpConnection::ptr &conn,
                                const HttpRequest::ptr &request) {
  ZHTTP_LOG_DEBUG("{} {} {}", method_to_string(request->method()),
                  request->path(), version_to_string(request->version()));

  // 把对端地址补进请求对象，便于日志、鉴权、限流等上层逻辑直接读取。
  if (conn && conn->socket()) {
    auto remote_addr = conn->socket()->get_remote_address();
    if (remote_addr) {
      request->set_remote_addr(remote_addr->to_string());
    }
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
  if (conn->send(response_str.data(), response_str.size()) < 0) {
    ZHTTP_LOG_WARN("Send HTTP response failed: fd={}", conn->fd());
    return false;
  }

  ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                  status_to_string(response.status_code()));

  return response.is_keep_alive();
}

} // namespace zhttp
