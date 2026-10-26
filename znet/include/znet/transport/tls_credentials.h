#ifndef ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_TLS_CREDENTIALS_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_TLS_CREDENTIALS_H_

#include "znet/transport/byte_stream.h"
#include <memory>
#include <string>

namespace znet {
class Socket;

/**
 * @brief 加载后不再修改的服务器凭据；SSL 会话持有 OpenSSL 对上下文的引用，
 * 凭据对象销毁不会单独使已创建流的 SSL 上下文失效。
 *
 * @note 仅支持 TLS 1.2 及以上版本；不支持客户端
 */
class TlsCredentials {
  public:
    static Result<TlsCredentials> load(const std::string &certificate,
                                       const std::string &private_key);
    ~TlsCredentials();
    TlsCredentials(TlsCredentials &&) noexcept;
    TlsCredentials &operator=(TlsCredentials &&) noexcept;
    TlsCredentials(const TlsCredentials &) = delete;
    TlsCredentials &operator=(const TlsCredentials &) = delete;

    /**
     * @brief 创建 TLS 流；流的生命周期不应超过凭据对象。
     * @note 仅支持 SOCK_STREAM 套接字；套接字必须在调用前绑定或连接。
     */
    Result<std::unique_ptr<ByteStream>> make_stream(Socket socket) const;

  private:
    struct Impl;
    explicit TlsCredentials(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace znet

#endif
