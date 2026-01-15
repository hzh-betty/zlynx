#include "https_server.h"

#include "buff.h"
#include "http_parser.h"
#include "tcp_connection.h"
#include "zhttp_logger.h"

#include <openssl/ssl.h>

namespace zhttp {

HttpsServer::HttpsServer(zcoroutine::IoScheduler::ptr io_worker,
                         zcoroutine::IoScheduler::ptr accept_worker)
    : HttpServer(std::move(io_worker), std::move(accept_worker)) {
  set_name("zhttp/1.0 (HTTPS)");
}

HttpsServer::~HttpsServer() = default;

bool HttpsServer::set_ssl_certificate(const std::string &cert_file,
                                      const std::string &key_file) {
  ssl_ctx_ = SslContext::create_server(cert_file, key_file);
  return ssl_ctx_ != nullptr;
}

void HttpsServer::handle_client(znet::TcpConnectionPtr conn) {
  if (!ssl_ctx_) {
    ZHTTP_LOG_ERROR("SSL context not initialized");
    conn->force_close();
    return;
  }

  ZHTTP_LOG_DEBUG("New HTTPS connection: {}", conn->name());

  // 创建 SSL 会话
  SSL *ssl = ssl_ctx_->create_ssl(conn->socket()->fd());
  if (!ssl) {
    ZHTTP_LOG_ERROR("Failed to create SSL session");
    conn->force_close();
    return;
  }

  SslSession session(ssl);

  // 执行 SSL 握手
  if (!session.accept()) {
    ZHTTP_LOG_ERROR("SSL handshake failed");
    conn->force_close();
    return;
  }

  ZHTTP_LOG_DEBUG("SSL handshake successful: {}", conn->name());

  // 设置关闭回调
  conn->set_close_callback([](const znet::TcpConnectionPtr &c) {
    ZHTTP_LOG_DEBUG("HTTPS connection closed: {}", c->name());
  });

  // 连接建立
  conn->connect_established();

  // HTTP 解析和处理
  HttpParser parser;
  std::vector<char> read_buffer(8192);

  while (conn->connected()) {
    // 通过 SSL 读取数据
    ssize_t n = session.read(read_buffer.data(), read_buffer.size());
    if (n < 0) {
      ZHTTP_LOG_WARN("SSL read error");
      break;
    } else if (n == 0) {
      // 需要重试或连接关闭
      continue;
    }

    // 将数据写入解析缓冲区
    conn->input_buffer()->append(read_buffer.data(), static_cast<size_t>(n));

    // 解析请求
    while (conn->input_buffer()->readable_bytes() > 0) {
      ParseResult result = parser.parse(conn->input_buffer());

      if (result == ParseResult::COMPLETE) {
        // 创建响应
        HttpResponse response;
        response.set_version(parser.request()->version());
        response.set_keep_alive(parser.request()->is_keep_alive());
        response.header("Server", "zhttp/1.0 (HTTPS)");

        // 路由处理
        router().route(parser.request(), response);

        // 通过 SSL 发送响应
        std::string response_str = response.serialize();
        session.write(response_str.data(), response_str.size());

        ZHTTP_LOG_DEBUG(
            "HTTPS {} {} -> {}", method_to_string(parser.request()->method()),
            parser.request()->path(), static_cast<int>(response.status_code()));

        // 检查 Keep-Alive
        if (!parser.request()->is_keep_alive()) {
          session.shutdown();
          return;
        }

        parser.reset();
      } else if (result == ParseResult::NEED_MORE) {
        break;
      } else if (result == ParseResult::ERROR) {
        ZHTTP_LOG_WARN("HTTPS parse error: {}", parser.error());
        HttpResponse response;
        response.status(HttpStatus::BAD_REQUEST)
            .content_type("text/plain")
            .body("Bad Request");
        response.set_keep_alive(false);
        std::string response_str = response.serialize();
        session.write(response_str.data(), response_str.size());
        session.shutdown();
        return;
      }
    }
  }

  session.shutdown();
}

} // namespace zhttp
