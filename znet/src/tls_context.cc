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

// OpenSSL 相关实现集中在本文件：
// - 进程级初始化（一次）。
// - TLS 通道读写/握手的状态机封装。
// - 服务端 TLS context 构建与证书加载。

struct OpenSslInitializer {
    OpenSslInitializer() {
        // OpenSSL 1.1+ 大多可自动初始化，这里显式调用保证行为稳定。
        OPENSSL_init_ssl(0, nullptr);
        OPENSSL_init_crypto(0, nullptr);
        SSL_load_error_strings();
    }
};

OpenSslInitializer kOpenSslInitializer;

std::string last_ssl_error_string() {
    // OpenSSL 错误队列是线程本地的，取一次就会前移，
    // 因此每次只拼接“最近一个”错误，避免与其他路径串台。
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

        // 握手是“需要可读/可写事件驱动的循环过程”：
        // SSL_accept 可能多次返回 WANT_READ/WANT_WRITE，
        // 外层需等待 fd 就绪后重试。
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
                    // wait_callback 失败通常是超时或外层取消。
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

        // SSL_read 参数是 int，超大请求需要截断到 int 上限。
        while (true) {
            ERR_clear_error();
            const int n = SSL_read(ssl_, buffer, chunk);
            if (n > 0) {
                return static_cast<ssize_t>(n);
            }

            const int ssl_error = SSL_get_error(ssl_, n);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                // 对端发送 close_notify，视作 TLS 层 EOF。
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
                // 系统调用层已给出 errno，直接透传更有诊断价值。
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
        // 循环直到全部写完或遇到不可恢复错误，
        // 保持“部分写成功”语义，便于上层决定是否重试。
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
                // 对端已关闭写方向，后续发送不可继续。
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

        // SSL_shutdown 可能需要双向往返；返回 0 并不代表失败，
        // 对本项目的“尽力关闭”语义，ret==0 也可直接返回。
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

        // SSL 对象和 socket fd 绑定后，后续握手/读写都基于该 fd。
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
    // TLS_server_method 支持协商，后续再通过 min proto 限制最低版本。
    SSL_CTX *raw_ctx = SSL_CTX_new(TLS_server_method());
    if (!raw_ctx) {
        if (error) {
            *error = "SSL_CTX_new failed: " + last_ssl_error_string();
        }
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(raw_ctx, TLS1_2_VERSION);
    // 证书和私钥加载顺序固定：先证书，再私钥，再做匹配校验。

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
