#ifndef ZLYNX_ZNET_INCLUDE_ZNET_ENDPOINT_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_ENDPOINT_H_

#include "znet/error.h"
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <vector>

namespace znet {
/**
 * @brief 拥有经校验的地址值；原生地址接口返回只读借用指针。
 */
class Endpoint {
  public:
    static Result<Endpoint> ipv4(std::string_view ip, uint16_t port = 0);
    static Result<Endpoint> ipv6(std::string_view ip, uint16_t port = 0);

    /**
     * @brief 首字节为 NUL 时表示 Linux 抽象命名空间地址；空路径表示
     * 未命名端点。
     */
    static Result<Endpoint> unix_domain(std::string_view path);

    /**
     * @brief 从原生地址创建；size 必须至少为 sockaddr_storage 的大小。
     * @note 仅支持 AF_INET、AF_INET6 和 AF_UNIX；Unix 地址长度可能小于
     * sockaddr_un 的大小。
     */
    static Result<Endpoint> from_native(const sockaddr *address,
                                        socklen_t size);

    int family() const { return storage_.ss_family; }

    uint16_t port() const;
    std::string to_string() const;

    const sockaddr *native_address() const {
        return reinterpret_cast<const sockaddr *>(&storage_);
    }

    socklen_t native_size() const { return size_; }

  private:
    Endpoint() = default;
    sockaddr_storage storage_{};
    socklen_t size_ = 0;
};

/**
 * @brief 执行阻塞地址解析；应在配置阶段、I/O 工作线程之外调用。
 */
Result<std::vector<Endpoint>> resolve_endpoints(std::string_view host,
                                                uint16_t port = 0,
                                                int family = AF_UNSPEC);
} // namespace znet

#endif
