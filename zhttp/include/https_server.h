#ifndef ZHTTP_HTTPS_SERVER_H_
#define ZHTTP_HTTPS_SERVER_H_

#include "http_server.h"
#include "ssl_context.h"

namespace zhttp {

/**
 * @brief HTTPS 服务器
 * @details
 * HttpsServer 在 HttpServer 的 HTTP 处理流程之上增加 TLS 握手和加密传输，
 * 业务层仍然按普通 HttpServer 的方式注册路由和处理中间件。
 */
class HttpsServer : public HttpServer {
public:
  using ptr = std::shared_ptr<HttpsServer>;

  /**
   * @brief 构造函数
   * @param io_worker IO 调度器
   * @param accept_worker Accept 调度器（可选）
   */
  HttpsServer(zcoroutine::IoScheduler::ptr io_worker,
              zcoroutine::IoScheduler::ptr accept_worker = nullptr);

  ~HttpsServer() override;

  /**
   * @brief 设置 SSL 证书
   * @param cert_file 证书文件路径
   * @param key_file 私钥文件路径
   * @return 是否成功
   */
  bool set_ssl_certificate(const std::string &cert_file,
                           const std::string &key_file);

protected:
  /**
   * @brief 处理新连接（带 SSL 握手）
   * @param conn TCP 连接对象
   * @details
   * 新连接建立后需要先完成 TLS 握手，握手成功后才会进入上层 HTTP 解析流程。
   */
  void handle_client(znet::TcpConnectionPtr conn) override;

private:
  // 共享的服务端 SSL 上下文，持有证书和 TLS 配置。
  SslContext::ptr ssl_ctx_;
};

} // namespace zhttp

#endif // ZHTTP_HTTPS_SERVER_H_
