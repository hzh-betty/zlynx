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
    Result<std::unique_ptr<ByteStream>> make_stream(Socket socket) const;

  private:
    struct Impl;
    explicit TlsCredentials(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace znet

#endif
