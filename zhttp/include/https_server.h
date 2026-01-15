#ifndef ZHTTP_HTTPS_SERVER_H_
#define ZHTTP_HTTPS_SERVER_H_

#include "http_server.h"
#include "ssl_context.h"

namespace zhttp {

/**
 * @brief HTTPS 服务器
 * 继承 HttpServer，增加 SSL/TLS 支持
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
   */
  void handle_client(znet::TcpConnectionPtr conn) override;

private:
  SslContext::ptr ssl_ctx_;
};

} // namespace zhttp

#endif // ZHTTP_HTTPS_SERVER_H_
