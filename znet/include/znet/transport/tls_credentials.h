#pragma once

#include "znet/transport/byte_stream.h"
#include <memory>
#include <string>

namespace znet {
class Socket;

// Immutable server credentials. SSL sessions hold OpenSSL's own reference to
// the context, so a stream remains valid after its credentials are destroyed.
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
