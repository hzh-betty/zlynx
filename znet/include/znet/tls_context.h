#ifndef ZNET_TLS_CONTEXT_H_
#define ZNET_TLS_CONTEXT_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace znet {

/**
 * @brief TLS 通道抽象。
 *
 * TcpConnection 仅依赖该接口，不感知具体 TLS 后端（如 OpenSSL）。
 */
class TlsChannel {
  public:
    using WaitCallback =
        std::function<bool(bool wait_for_write, uint32_t timeout_ms)>;

    virtual ~TlsChannel() = default;

    virtual bool handshake(uint32_t timeout_ms,
                           const WaitCallback &wait_callback) = 0;

    virtual ssize_t read(void *buffer, size_t length, uint32_t timeout_ms,
                         const WaitCallback &wait_callback) = 0;

    virtual ssize_t write(const void *buffer, size_t length,
                          uint32_t timeout_ms,
                          const WaitCallback &wait_callback) = 0;

    virtual void shutdown(uint32_t timeout_ms,
                          const WaitCallback &wait_callback) = 0;
};

/**
 * @brief TLS 上下文抽象。
 *
 * 负责按连接创建 TLS 通道对象。
 */
class TlsContext {
  public:
    using ptr = std::shared_ptr<TlsContext>;

    virtual ~TlsContext() = default;

    virtual std::unique_ptr<TlsChannel> create_server_channel(int fd) const = 0;
};

/**
 * @brief 创建 OpenSSL 服务端 TLS 上下文。
 * @param cert_file 证书文件。
 * @param key_file 私钥文件。
 * @param error 失败时可选返回错误文本。
 */
TlsContext::ptr create_server_tls_context_openssl(const std::string &cert_file,
                                                  const std::string &key_file,
                                                  std::string *error = nullptr);

} // namespace znet

#endif // ZNET_TLS_CONTEXT_H_
