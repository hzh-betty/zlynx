#ifndef ZNET_ADDRESS_H_
#define ZNET_ADDRESS_H_

#include <arpa/inet.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>

namespace znet {

/**
 * @brief 网络地址抽象基类
 * 支持 IPv4/IPv6/Unix Domain Socket
 */
class Address {
public:
  using ptr = std::shared_ptr<Address>;

  virtual ~Address() = default;

  /**
   * @brief 获取地址族
   * @return AF_INET/AF_INET6/AF_UNIX
   */
  virtual int family() const = 0;

  /**
   * @brief 获取 sockaddr 指针
   */
  virtual const sockaddr *sockaddr_ptr() const = 0;
  virtual sockaddr *sockaddr_ptr() = 0;

  /**
   * @brief 获取 sockaddr 长度
   */
  virtual socklen_t sockaddr_len() const = 0;

  /**
   * @brief 转换为字符串表示
   */
  virtual std::string to_string() const = 0;

  /**
   * @brief 域名解析，返回所有解析出的地址
   * @param host 主机名或 IP 地址
   * @param port 端口号
   * @param family 地址族，0 表示不限制
   * @return 地址列表
   */
  static std::vector<Address::ptr> lookup(const std::string &host,
                                          uint16_t port = 0, int family = 0);

  /**
   * @brief 从 sockaddr 创建 Address 对象
   */
  static Address::ptr create(const sockaddr *addr, socklen_t addrlen);
};

/**
 * @brief IP 地址抽象基类（IPv4 和 IPv6 的公共接口）
 */
class IPAddress : public Address {
public:
  using ptr = std::shared_ptr<IPAddress>;

  /**
   * @brief 获取端口号
   */
  virtual uint16_t port() const = 0;

  /**
   * @brief 设置端口号
   */
  virtual void set_port(uint16_t port) = 0;
};

/**
 * @brief IPv4 地址
 */
class IPv4Address : public IPAddress {
public:
  using ptr = std::shared_ptr<IPv4Address>;

  /**
   * @brief 构造函数
   * @param ip IP 地址，点分十进制格式，如 "192.168.1.1"
   * @param port 端口号
   */
  IPv4Address(const std::string &ip = "0.0.0.0", uint16_t port = 0);

  /**
   * @brief 从 sockaddr_in 构造
   */
  IPv4Address(const sockaddr_in &addr);

  int family() const override { return AF_INET; }

  const sockaddr *sockaddr_ptr() const override {
    return reinterpret_cast<const sockaddr *>(&addr_);
  }

  sockaddr *sockaddr_ptr() override {
    return reinterpret_cast<sockaddr *>(&addr_);
  }

  socklen_t sockaddr_len() const override { return sizeof(addr_); }

  std::string to_string() const override;

  uint16_t port() const override;

  void set_port(uint16_t port) override;

private:
  sockaddr_in addr_;
};

/**
 * @brief IPv6 地址
 */
class IPv6Address : public IPAddress {
public:
  using ptr = std::shared_ptr<IPv6Address>;

  /**
   * @brief 构造函数
   * @param ip IPv6 地址，如 "::1"
   * @param port 端口号
   */
  IPv6Address(const std::string &ip = "::", uint16_t port = 0);

  /**
   * @brief 从 sockaddr_in6 构造
   */
  IPv6Address(const sockaddr_in6 &addr);

  int family() const override { return AF_INET6; }

  const sockaddr *sockaddr_ptr() const override {
    return reinterpret_cast<const sockaddr *>(&addr_);
  }

  sockaddr *sockaddr_ptr() override {
    return reinterpret_cast<sockaddr *>(&addr_);
  }

  socklen_t sockaddr_len() const override { return sizeof(addr_); }

  std::string to_string() const override;

  uint16_t port() const override;

  void set_port(uint16_t port) override;

private:
  sockaddr_in6 addr_;
};

/**
 * @brief Unix Domain Socket 地址
 */
class UnixAddress : public Address {
public:
  using ptr = std::shared_ptr<UnixAddress>;

  /**
   * @brief 构造函数
   * @param path Socket 文件路径
   */
  UnixAddress(const std::string &path = "");

  /**
   * @brief 从 sockaddr_un 构造
   */
  UnixAddress(const sockaddr_un &addr);

  int family() const override { return AF_UNIX; }

  const sockaddr *sockaddr_ptr() const override {
    return reinterpret_cast<const sockaddr *>(&addr_);
  }

  sockaddr *sockaddr_ptr() override {
    return reinterpret_cast<sockaddr *>(&addr_);
  }

  socklen_t sockaddr_len() const override { return sizeof(addr_); }

  std::string to_string() const override;

  /**
   * @brief 设置路径
   */
  void set_path(const std::string &path);

  /**
   * @brief 获取路径
   */
  std::string path() const;

private:
  sockaddr_un addr_;
};

} // namespace znet

#endif // ZNET_ADDRESS_H_
