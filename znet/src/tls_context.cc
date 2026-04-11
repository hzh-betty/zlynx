#include "znet/tls_context.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "znet/znet_logger.h"

namespace znet {

namespace {

struct OpenSslInitializer {
    OpenSslInitializer() {
        OPENSSL_init_ssl(0, nullptr);
        OPENSSL_init_crypto(0, nullptr);
        SSL_load_error_strings();
    }
};

OpenSslInitializer kOpenSslInitializer;

std::string last_ssl_error_string() {
    const unsigned long err = ERR_get_error();
    if (err == 0) {
        return "";
    }

    char buf[256] = {0};
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

class OpenSslTlsChannel : public TlsChannel {
  public:
    explicit OpenSslTlsChannel(SSL *ssl) : ssl_(ssl) {}

    ~OpenSslTlsChannel() override {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
    }

    bool handshake(uint32_t timeout_ms,
                   const WaitCallback &wait_callback) override {
        if (!ssl_) {
            errno = EINVAL;
            return false;
        }

        while (true) {
            ERR_clear_error();
            const int ret = SSL_accept(ssl_);
            if (ret == 1) {
                return true;
            }

            const int ssl_error = SSL_get_error(ssl_, ret);
            if (ssl_error == SSL_ERROR_WANT_READ ||
                ssl_error == SSL_ERROR_WANT_WRITE) {
                if (!wait_callback ||
                    !wait_callback(ssl_error == SSL_ERROR_WANT_WRITE,
                                   timeout_ms)) {
                    if (errno == 0) {
                        errno = ETIMEDOUT;
                    }
                    return false;
                }
                continue;
            }

            if (ssl_error == SSL_ERROR_SYSCALL && errno != 0) {
                return false;
            }

            errno = EPROTO;
            return false;
        }
    }

    ssize_t read(void *buffer, size_t length, uint32_t timeout_ms,
                 const WaitCallback &wait_callback) override {
        if (!ssl_ || !buffer) {
            errno = EINVAL;
            return -1;
        }

        const int chunk = static_cast<int>(std::min(
            length, static_cast<size_t>(std::numeric_limits<int>::max())));

        while (true) {
            ERR_clear_error();
            const int n = SSL_read(ssl_, buffer, chunk);
            if (n > 0) {
                return static_cast<ssize_t>(n);
            }

            const int ssl_error = SSL_get_error(ssl_, n);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                return 0;
            }

            if (ssl_error == SSL_ERROR_WANT_READ ||
                ssl_error == SSL_ERROR_WANT_WRITE) {
                if (!wait_callback ||
                    !wait_callback(ssl_error == SSL_ERROR_WANT_WRITE,
                                   timeout_ms)) {
                    if (errno == 0) {
                        errno = ETIMEDOUT;
                    }
                    return -1;
                }
                continue;
            }

            if (ssl_error == SSL_ERROR_SYSCALL && errno != 0) {
                return -1;
            }

            errno = EIO;
            return -1;
        }
    }

    ssize_t write(const void *buffer, size_t length, uint32_t timeout_ms,
                  const WaitCallback &wait_callback) override {
        if (!ssl_ || (!buffer && length > 0)) {
            errno = EINVAL;
            return -1;
        }

        size_t sent = 0;
        while (sent < length) {
            const size_t remaining = length - sent;
            const int chunk = static_cast<int>(
                std::min(remaining,
                         static_cast<size_t>(std::numeric_limits<int>::max())));

            ERR_clear_error();
            const int n = SSL_write(
                ssl_, static_cast<const char *>(buffer) + sent, chunk);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }

            const int ssl_error = SSL_get_error(ssl_, n);
            if (ssl_error == SSL_ERROR_WANT_READ ||
                ssl_error == SSL_ERROR_WANT_WRITE) {
                if (!wait_callback ||
                    !wait_callback(ssl_error == SSL_ERROR_WANT_WRITE,
                                   timeout_ms)) {
                    if (errno == 0) {
                        errno = ETIMEDOUT;
                    }
                    return sent > 0 ? static_cast<ssize_t>(sent) : -1;
                }
                continue;
            }

            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                errno = EPIPE;
                return sent > 0 ? static_cast<ssize_t>(sent) : -1;
            }

            if (ssl_error == SSL_ERROR_SYSCALL && errno != 0) {
                return sent > 0 ? static_cast<ssize_t>(sent) : -1;
            }

            errno = EIO;
            return sent > 0 ? static_cast<ssize_t>(sent) : -1;
        }

        return static_cast<ssize_t>(sent);
    }

    void shutdown(uint32_t timeout_ms,
                  const WaitCallback &wait_callback) override {
        if (!ssl_) {
            return;
        }

        while (true) {
            ERR_clear_error();
            const int ret = SSL_shutdown(ssl_);
            if (ret == 1 || ret == 0) {
                return;
            }

            const int ssl_error = SSL_get_error(ssl_, ret);
            if (ssl_error == SSL_ERROR_WANT_READ ||
                ssl_error == SSL_ERROR_WANT_WRITE) {
                if (!wait_callback ||
                    !wait_callback(ssl_error == SSL_ERROR_WANT_WRITE,
                                   timeout_ms)) {
                    return;
                }
                continue;
            }

            return;
        }
    }

  private:
    SSL *ssl_;
};

class OpenSslServerTlsContext : public TlsContext {
  public:
    explicit OpenSslServerTlsContext(std::shared_ptr<SSL_CTX> ctx)
        : ctx_(std::move(ctx)) {}

    std::unique_ptr<TlsChannel> create_server_channel(int fd) const override {
        if (!ctx_ || fd < 0) {
            errno = EINVAL;
            return nullptr;
        }

        ERR_clear_error();
        SSL *ssl = SSL_new(ctx_.get());
        if (!ssl) {
            errno = EIO;
            return nullptr;
        }

        if (SSL_set_fd(ssl, fd) != 1) {
            SSL_free(ssl);
            errno = EIO;
            return nullptr;
        }

        return std::unique_ptr<TlsChannel>(new OpenSslTlsChannel(ssl));
    }

  private:
    std::shared_ptr<SSL_CTX> ctx_;
};

} // namespace

TlsContext::ptr create_server_tls_context_openssl(const std::string &cert_file,
                                                  const std::string &key_file,
                                                  std::string *error) {
    ERR_clear_error();
    SSL_CTX *raw_ctx = SSL_CTX_new(TLS_server_method());
    if (!raw_ctx) {
        if (error) {
            *error = "SSL_CTX_new failed: " + last_ssl_error_string();
        }
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(raw_ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(raw_ctx, cert_file.c_str(),
                                     SSL_FILETYPE_PEM) <= 0) {
        if (error) {
            *error = "load certificate failed: " + last_ssl_error_string();
        }
        SSL_CTX_free(raw_ctx);
        return nullptr;
    }

    if (SSL_CTX_use_PrivateKey_file(raw_ctx, key_file.c_str(),
                                    SSL_FILETYPE_PEM) <= 0) {
        if (error) {
            *error = "load private key failed: " + last_ssl_error_string();
        }
        SSL_CTX_free(raw_ctx);
        return nullptr;
    }

    if (!SSL_CTX_check_private_key(raw_ctx)) {
        if (error) {
            *error = "certificate and private key mismatch";
        }
        SSL_CTX_free(raw_ctx);
        return nullptr;
    }

    auto ctx = std::shared_ptr<SSL_CTX>(raw_ctx, [](SSL_CTX *p) {
        if (p) {
            SSL_CTX_free(p);
        }
    });

    ZNET_LOG_INFO("create_server_tls_context_openssl success: cert={}, key={}",
                  cert_file, key_file);

    return std::make_shared<OpenSslServerTlsContext>(std::move(ctx));
}

} // namespace znet
