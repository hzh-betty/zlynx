#ifndef ZHTTP_SSL_CONTEXT_H_
#define ZHTTP_SSL_CONTEXT_H_

#include <memory>
#include <string>

// 前向声明 OpenSSL 类型
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace zhttp {

/**
 * @brief SSL 上下文管理
 * 封装 OpenSSL 的 SSL_CTX
 */
class SslContext {
public:
  using ptr = std::shared_ptr<SslContext>;

  /**
   * @brief 创建服务端 SSL 上下文
   * @param cert_file 证书文件路径
   * @param key_file 私钥文件路径
   * @return SSL 上下文智能指针，失败返回 nullptr
   */
  static SslContext::ptr create_server(const std::string &cert_file,
                                       const std::string &key_file);

  /**
   * @brief 创建客户端 SSL 上下文
   * @return SSL 上下文智能指针
   */
  static SslContext::ptr create_client();

  ~SslContext();

  /**
   * @brief 获取底层 SSL_CTX 指针
   */
  SSL_CTX *native_handle() const { return ctx_; }

  /**
   * @brief 创建 SSL 会话
   * @param fd Socket 文件描述符
   * @return SSL 会话指针，失败返回 nullptr
   */
  SSL *create_ssl(int fd);

private:
  SslContext();
  bool init_server(const std::string &cert_file, const std::string &key_file);
  bool init_client();

private:
  SSL_CTX *ctx_ = nullptr;
};

/**
 * @brief SSL 会话 RAII 包装
 */
class SslSession {
public:
  /**
   * @brief 构造函数
   * @param ssl SSL 会话指针
   */
  explicit SslSession(SSL *ssl);

  ~SslSession();

  // 禁止拷贝
  SslSession(const SslSession &) = delete;
  SslSession &operator=(const SslSession &) = delete;

  // 允许移动
  SslSession(SslSession &&other) noexcept;
  SslSession &operator=(SslSession &&other) noexcept;

  /**
   * @brief 执行 SSL 握手（服务端）
   * @return 成功返回 true
   */
  bool accept();

  /**
   * @brief 执行 SSL 握手（客户端）
   * @return 成功返回 true
   */
  bool connect();

  /**
   * @brief 读取数据
   * @param buffer 缓冲区
   * @param len 缓冲区长度
   * @return 读取的字节数，-1 表示错误
   */
  ssize_t read(void *buffer, size_t len);

  /**
   * @brief 写入数据
   * @param buffer 数据缓冲区
   * @param len 数据长度
   * @return 写入的字节数，-1 表示错误
   */
  ssize_t write(const void *buffer, size_t len);

  /**
   * @brief 关闭 SSL 会话
   */
  void shutdown();

  /**
   * @brief 获取底层 SSL 指针
   */
  SSL *native_handle() const { return ssl_; }

  /**
   * @brief 是否有效
   */
  bool is_valid() const { return ssl_ != nullptr; }

private:
  SSL *ssl_ = nullptr;
};

} // namespace zhttp

#endif // ZHTTP_SSL_CONTEXT_H_
