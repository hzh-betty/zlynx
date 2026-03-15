#include "https_server.h"

#include "buff.h"
#include "http_parser.h"
#include "tcp_connection.h"
#include "zhttp_logger.h"

#include <openssl/ssl.h>
#include <vector>

namespace zhttp {

namespace {

constexpr size_t kHttpsReadBufferSize = 8192;

} // namespace

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

  HttpParser parser;
  std::vector<char> read_buffer(kHttpsReadBufferSize);

  // 这里不依赖 TcpConnection 的 message_callback 读流程，而是由 TLS 会话驱动。
  // 原因是 TLS 需要先解密后再交给 HTTP 解析器，明文/密文读取路径不能混用。
  while (!conn->disconnected()) {
    ssize_t n = session.read(read_buffer.data(), read_buffer.size());
    if (n < 0) {
      ZHTTP_LOG_WARN("SSL read error");
      break;
    } else if (n == 0) {
      continue;
    }

    conn->input_buffer()->append(read_buffer.data(), static_cast<size_t>(n));
    if (!on_message(conn, parser, session, conn->input_buffer())) {
      ZHTTP_LOG_DEBUG("HTTPS connection closed: {}", conn->name());
      return;
    }
  }

  session.shutdown();
  conn->force_close();
  ZHTTP_LOG_DEBUG("HTTPS connection closed: {}", conn->name());
}

bool HttpsServer::on_message(const znet::TcpConnectionPtr &conn,
                             HttpParser &parser, SslSession &session,
                             znet::Buffer *buffer) {
  while (buffer->readable_bytes() > 0) {
    ParseResult result = parser.parse(buffer);

    if (result == ParseResult::COMPLETE) {
      const bool keep_alive = handle_request(conn, parser.request(), session);

      if (!keep_alive) {
        session.shutdown();
        conn->force_close();
        return false;
      }

      parser.reset();
    } else if (result == ParseResult::NEED_MORE) {
      return true;
    } else if (result == ParseResult::ERROR) {
      ZHTTP_LOG_WARN("HTTPS parse error: {}", parser.error());
      HttpResponse response;
      response.status(HttpStatus::BAD_REQUEST)
          .content_type("text/plain")
          .body("Bad Request: " + parser.error());
      response.set_keep_alive(false);

      const std::string response_str = response.serialize();
      write_all(session, response_str.data(), response_str.size());
      session.shutdown();
      conn->force_close();
      return false;
    }
  }

  return true;
}

bool HttpsServer::handle_request(const znet::TcpConnectionPtr &conn,
                                 const HttpRequest::ptr &request,
                                 SslSession &session) {
  ZHTTP_LOG_DEBUG("{} {} {}", method_to_string(request->method()),
                  request->path(), version_to_string(request->version()));

  if (conn && conn->peer_address()) {
    request->set_remote_addr(conn->peer_address()->to_string());
  }

  HttpResponse response;
  response.set_version(request->version());
  response.set_keep_alive(request->is_keep_alive());
  response.header("Server", name());

  router().route(request, response);

  const std::string response_str = response.serialize();
  if (!write_all(session, response_str.data(), response_str.size())) {
    ZHTTP_LOG_WARN("HTTPS write error: {} {}", method_to_string(request->method()),
                   request->path());
    return false;
  }

  ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                  status_to_string(response.status_code()));

  return response.is_keep_alive();
}

bool HttpsServer::write_all(SslSession &session, const char *data,
                            size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const ssize_t n =
        session.write(data + sent, static_cast<size_t>(size - sent));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      continue;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

} // namespace zhttp
