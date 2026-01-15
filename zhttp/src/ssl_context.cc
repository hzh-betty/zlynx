#include "ssl_context.h"

#include "zhttp_logger.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace zhttp {

namespace {
// OpenSSL 全局初始化（程序启动时自动执行）
struct OpenSSLInitializer {
  OpenSSLInitializer() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
  }
  ~OpenSSLInitializer() { EVP_cleanup(); }
};
static OpenSSLInitializer g_ssl_initializer;

// 获取 OpenSSL 错误信息
std::string get_ssl_error() {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
  return std::string(buf);
}
} // namespace

// ===================== SslContext =====================

SslContext::SslContext() = default;

SslContext::~SslContext() {
  if (ctx_) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

SslContext::ptr SslContext::create_server(const std::string &cert_file,
                                          const std::string &key_file) {
  auto ctx = std::shared_ptr<SslContext>(new SslContext());
  if (!ctx->init_server(cert_file, key_file)) {
    return nullptr;
  }
  return ctx;
}

SslContext::ptr SslContext::create_client() {
  auto ctx = std::shared_ptr<SslContext>(new SslContext());
  if (!ctx->init_client()) {
    return nullptr;
  }
  return ctx;
}

bool SslContext::init_server(const std::string &cert_file,
                             const std::string &key_file) {
  // 创建 TLS 服务端上下文
  ctx_ = SSL_CTX_new(TLS_server_method());
  if (!ctx_) {
    ZHTTP_LOG_ERROR("Failed to create SSL context: {}", get_ssl_error());
    return false;
  }

  // 设置最低 TLS 版本为 TLS 1.2
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

  // 加载证书
  if (SSL_CTX_use_certificate_file(ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <=
      0) {
    ZHTTP_LOG_ERROR("Failed to load certificate {}: {}", cert_file,
                    get_ssl_error());
    return false;
  }

  // 加载私钥
  if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <=
      0) {
    ZHTTP_LOG_ERROR("Failed to load private key {}: {}", key_file,
                    get_ssl_error());
    return false;
  }

  // 验证私钥与证书匹配
  if (!SSL_CTX_check_private_key(ctx_)) {
    ZHTTP_LOG_ERROR("Private key does not match certificate");
    return false;
  }

  ZHTTP_LOG_INFO("SSL context initialized with cert: {}, key: {}", cert_file,
                 key_file);
  return true;
}

bool SslContext::init_client() {
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ctx_) {
    ZHTTP_LOG_ERROR("Failed to create SSL client context: {}", get_ssl_error());
    return false;
  }

  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  return true;
}

SSL *SslContext::create_ssl(int fd) {
  if (!ctx_) {
    return nullptr;
  }

  SSL *ssl = SSL_new(ctx_);
  if (!ssl) {
    ZHTTP_LOG_ERROR("Failed to create SSL: {}", get_ssl_error());
    return nullptr;
  }

  if (SSL_set_fd(ssl, fd) != 1) {
    ZHTTP_LOG_ERROR("Failed to set SSL fd: {}", get_ssl_error());
    SSL_free(ssl);
    return nullptr;
  }

  return ssl;
}

// ===================== SslSession =====================

SslSession::SslSession(SSL *ssl) : ssl_(ssl) {}

SslSession::~SslSession() {
  if (ssl_) {
    shutdown();
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
}

SslSession::SslSession(SslSession &&other) noexcept : ssl_(other.ssl_) {
  other.ssl_ = nullptr;
}

SslSession &SslSession::operator=(SslSession &&other) noexcept {
  if (this != &other) {
    if (ssl_) {
      shutdown();
      SSL_free(ssl_);
    }
    ssl_ = other.ssl_;
    other.ssl_ = nullptr;
  }
  return *this;
}

bool SslSession::accept() {
  if (!ssl_) {
    return false;
  }

  int ret = SSL_accept(ssl_);
  if (ret != 1) {
    int err = SSL_get_error(ssl_, ret);
    ZHTTP_LOG_ERROR("SSL accept failed, error code: {}", err);
    return false;
  }
  return true;
}

bool SslSession::connect() {
  if (!ssl_) {
    return false;
  }

  int ret = SSL_connect(ssl_);
  if (ret != 1) {
    int err = SSL_get_error(ssl_, ret);
    ZHTTP_LOG_ERROR("SSL connect failed, error code: {}", err);
    return false;
  }
  return true;
}

ssize_t SslSession::read(void *buffer, size_t len) {
  if (!ssl_) {
    return -1;
  }

  int ret = SSL_read(ssl_, buffer, static_cast<int>(len));
  if (ret <= 0) {
    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      return 0; // 非阻塞，需要重试
    }
    return -1;
  }
  return ret;
}

ssize_t SslSession::write(const void *buffer, size_t len) {
  if (!ssl_) {
    return -1;
  }

  int ret = SSL_write(ssl_, buffer, static_cast<int>(len));
  if (ret <= 0) {
    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      return 0; // 非阻塞，需要重试
    }
    return -1;
  }
  return ret;
}

void SslSession::shutdown() {
  if (ssl_) {
    SSL_shutdown(ssl_);
  }
}

} // namespace zhttp
