#include "zco/coroutine.h"
#include "zco/io/operations.h"
#include "znet/transport/socket.h"
#include "znet/transport/tls_credentials.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <mutex>
#include <openssl/err.h>
#include <openssl/ssl.h>

/*
 * SSL_CTX	保存服务器证书、私钥、协议版本等公共配置	每次 load() 创建一个
 * SSL	保存某条连接的握手、加解密等状态	每次 make_stream() 创建一个
 * 
    TlsCredentials
        └── Impl
            └── SSL_CTX
                    ↑          ↑
                SSL A      SSL B
                    │          │
            TlsStream A  TlsStream B
                    │          │
                Socket A   Socket B
*/

namespace znet {
namespace {
using Ssl = std::unique_ptr<SSL, decltype(&SSL_free)>;
using SslContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

std::string ssl_detail() {
    const auto code = ERR_get_error();
    if (!code)
        return {};
    char text[256]{};
    ERR_error_string_n(code, text, sizeof(text));
    return text;
}

Error tls_error(std::string operation) {
    return make_error(ErrorKind::protocol, std::errc::protocol_error,
                      std::move(operation), ssl_detail());
}

// recv() 收到密文 → BIO read 回调 → SSL_read() 解密 → 应用得到明文
// send() 发送明文 → SSL_write() 加密 → BIO write 回调 → 发送密文
const BIO_METHOD *socket_bio() {
    static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
        [] {
            BIO_METHOD *bio = BIO_meth_new(
                BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "znet socket");
            if (!bio)
                throw std::bad_alloc();
            BIO_meth_set_create(bio, [](BIO *b) {
                BIO_set_init(b, 1);
                return 1;
            });
            
            BIO_meth_set_destroy(bio, [](BIO *) { return 1; });
            
            BIO_meth_set_write(bio, [](BIO *b, const char *bytes, int size) {
                BIO_clear_retry_flags(b);
                const int fd = *static_cast<const int *>(BIO_get_data(b));
                // OpenSSL 的标准套接字 BIO 可能触发 SIGPIPE；此 BIO 使用 MSG_NOSIGNAL
                const int n =
                    static_cast<int>(::send(fd, bytes, size, MSG_NOSIGNAL));
                if (n < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    BIO_set_retry_write(b);
                return n;
            });
            BIO_meth_set_read(bio, [](BIO *b, char *bytes, int size) {
                BIO_clear_retry_flags(b);
                const int fd = *static_cast<const int *>(BIO_get_data(b));
                const int n = static_cast<int>(::recv(fd, bytes, size, 0));
                if (n < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    BIO_set_retry_read(b);
                return n;
            });

            // 对 BIO_CTRL_FLUSH 返回成功，这里没有额外的 BIO 缓冲需要刷新
            BIO_meth_set_ctrl(bio,
                              [](BIO *, int command, long, void *) -> long {
                                  return command == BIO_CTRL_FLUSH ? 1 : 0;
                              });
            return bio;
        }(),
        BIO_meth_free);
    return method.get();
}

class TlsStream final : public ByteStream {
  public:
    TlsStream(Socket socket, Ssl ssl)
        : socket_(std::move(socket)), fd_(socket_.native_handle()),
          ssl_(std::move(ssl)) {}

    /**
     * @brief 将 SSL 对象与套接字关联，并设置为服务器模式。
     * @note 仅支持 SOCK_STREAM 套接字；套接字必须在调用前绑定或连接。
     */
    Result<void> attach() {
        BIO *bio = BIO_new(socket_bio());
        if (!bio)
            return tls_error("create TLS socket BIO");

        // 给 BIO 关联自定义数据
        BIO_set_data(bio, &fd_);
        
        // 同一个 BIO 同时设为该 SSL 的读取通道和写入通道 
        SSL_set_bio(ssl_.get(), bio, bio);
        
        // 设置 SSL 为服务器模式，启用部分写入和可移动写缓冲区
        SSL_set_accept_state(ssl_.get());
        SSL_set_mode(ssl_.get(), SSL_MODE_ENABLE_PARTIAL_WRITE |
                                     SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        return {};
    }

    Result<void> start(zco::Deadline deadline) override {
        auto result = retry([&] { return SSL_accept(ssl_.get()); }, deadline,
                            "TLS handshake");
        if (!result)
            return result.error;
        if (result.eof)
            return make_error(ErrorKind::protocol, std::errc::protocol_error,
                              "TLS handshake", "peer closed during handshake");
        return {};
    }

    Transfer read_some(void *bytes, size_t size,
                       zco::Deadline deadline) override {
        if (!size)
            return {};
        if (!bytes)
            return {0,
                    make_error(ErrorKind::validation,
                               std::errc::invalid_argument, "TLS read"),
                    false};
        const int count = static_cast<int>(std::min(size, size_t{INT_MAX}));
        return retry([&] { return SSL_read(ssl_.get(), bytes, count); },
                     deadline, "TLS read");
    }

    Transfer write_some(const void *bytes, size_t size,
                        zco::Deadline deadline) override {
        if (!size)
            return {};
        if (!bytes)
            return {0,
                    make_error(ErrorKind::validation,
                               std::errc::invalid_argument, "TLS write"),
                    false};
        const int count = static_cast<int>(std::min(size, size_t{INT_MAX}));
        auto result = retry([&] { return SSL_write(ssl_.get(), bytes, count); },
                            deadline, "TLS write");
        if (result.eof)
            return {0, io_error("TLS write", EPIPE), false};
        return result;
    }

    Result<void> shutdown_write(zco::Deadline deadline) override {
        // 发送 close_notify 后返回，避免等待对端的关闭通知。
        auto result = retry([&] { return SSL_shutdown(ssl_.get()); }, deadline,
                            "TLS shutdown", true);
        if (!result)
            return result.error;
        return socket_.shutdown_write();
    }

    Result<void> close() override { return socket_.close(); }

    Result<Endpoint> local_endpoint() const override {
        return socket_.local_endpoint();
    }

    Result<Endpoint> remote_endpoint() const override {
        return socket_.remote_endpoint();
    }

    int native_handle() const override { return socket_.native_handle(); }

    bool encrypted() const override { return true; }

  private:
    template <class F>
    Transfer retry(F call, zco::Deadline deadline, const char *operation,
                   bool zero_success = false) {
        // 
        if (!zco::in_coroutine())
            return {0,
                    make_error(ErrorKind::runtime,
                               std::errc::operation_not_permitted, operation,
                               "IO requires a runtime task"),
                    false};
        

        for (;;) {
            if (deadline.expired(zco::Deadline::Clock::now()))
                return {0,
                        make_error(ErrorKind::runtime, std::errc::timed_out,
                                   operation),
                        false};
            int ssl_error;
            {
                // 对 SSL 对象的访问串行化，但两把锁均不跨协程等待持有，
                // 避免读取等待阻止写入继续。
                std::lock_guard<std::mutex> lock(ssl_mutex_);
                auto borrowed = socket_.descriptor().borrow();
                if (borrowed.fd() < 0)
                    return {0, io_error(operation, EBADF), false};
                ERR_clear_error();
                errno = 0;
                const int count = call();
                const int system_error = errno;
                if (count > 0 || (count == 0 && zero_success))
                    return {static_cast<size_t>(count), {}, false};
                
                // SSL_read/SSL_write 返回 <= 0，检查错误类型
                ssl_error = SSL_get_error(ssl_.get(), count);
                if (ssl_error == SSL_ERROR_ZERO_RETURN)
                    return {0, {}, true};
                if (ssl_error != SSL_ERROR_WANT_READ &&
                    ssl_error != SSL_ERROR_WANT_WRITE) {
                    if (ssl_error == SSL_ERROR_SYSCALL && system_error)
                        return {0, io_error(operation, system_error), false};
                    return {0, tls_error(operation), false};
                }
            }

            // SSL 需要等待可读或可写，可能挂起当前协程
            auto ready = zco::io::wait_ready(socket_.descriptor(),
                                             ssl_error == SSL_ERROR_WANT_READ
                                                 ? zco::io::Interest::read
                                                 : zco::io::Interest::write,
                                             deadline);
            if (!ready)
                return {0, runtime_error(operation, ready.error()), false};
        }
    }

    Socket socket_;
    int fd_;
    Ssl ssl_;
    std::mutex ssl_mutex_;
};
} // namespace

struct TlsCredentials::Impl {
    explicit Impl(SslContext context) : context(std::move(context)) {}

    SslContext context;
};

TlsCredentials::TlsCredentials(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsCredentials::~TlsCredentials() = default;
TlsCredentials::TlsCredentials(TlsCredentials &&) noexcept = default;
TlsCredentials &TlsCredentials::operator=(TlsCredentials &&) noexcept = default;

Result<TlsCredentials> TlsCredentials::load(const std::string &certificate,
                                            const std::string &private_key) {
    // 证书和私钥路径不应包含 NUL 字节，避免被 OpenSSL 解释为字符串结束。
    if (certificate.empty() || private_key.empty() ||
        certificate.find('\0') != std::string::npos ||
        private_key.find('\0') != std::string::npos)
        return make_error(ErrorKind::validation, std::errc::invalid_argument,
                          "TLS credentials", "invalid certificate/key path");
    
    ERR_clear_error(); // 清除 OpenSSL 错误队列，避免后续调用被之前的错误污染。

    // 创建 SSL_CTX 并加载证书和私钥
    SslContext context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context)
        return tls_error("create TLS credentials");
    if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1)
        return tls_error("set TLS minimum version");
    if (SSL_CTX_use_certificate_chain_file(context.get(),
                                           certificate.c_str()) != 1)
        return tls_error("load TLS certificate");
    if (SSL_CTX_use_PrivateKey_file(context.get(), private_key.c_str(),
                                    SSL_FILETYPE_PEM) != 1)
        return tls_error("load TLS private key");
    if (SSL_CTX_check_private_key(context.get()) != 1)
        return tls_error("validate TLS private key");
    return TlsCredentials(std::make_unique<Impl>(std::move(context)));
}

Result<std::unique_ptr<ByteStream>>
TlsCredentials::make_stream(Socket socket) const {
    // 套接字必须是 SOCK_STREAM 类型，TLS 不支持数据报套接字。
    if (!impl_)
        throw std::logic_error("Using moved-from TLS credentials");
    auto type = socket.option<int>(SOL_SOCKET, SO_TYPE);
    if (!type)
        return type.error();
    if (type.value() != SOCK_STREAM)
        return make_error(ErrorKind::validation, std::errc::invalid_argument,
                          "TLS stream");

    ERR_clear_error();

    // 创建 SSL 对象并关联套接字
    Ssl ssl(SSL_new(impl_->context.get()), SSL_free);
    if (!ssl)
        return tls_error("create TLS stream");
    auto stream =
        std::make_unique<TlsStream>(std::move(socket), std::move(ssl));

    auto attached = stream->attach();
    if (!attached)
        return attached.error();
    return std::unique_ptr<ByteStream>(std::move(stream));
}
} // namespace znet
