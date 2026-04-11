#include "znet/address.h"

#include <cstdio>
#include <cstring>
#include <netdb.h>

#include "znet/znet_logger.h"

namespace znet {

// 通过 getaddrinfo 解析 host:port，并将结果统一转换为 Address 派生对象。
std::vector<Address::ptr> Address::lookup(const std::string &host,
                                          uint16_t port, int family) {
    std::vector<Address::ptr> result;

    // hints 仅限制地址族与 socket 类型，协议由系统自行选择。
    struct addrinfo hints, *res, *curr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family; // AF_INET, AF_INET6, 或 0 (不限制)
    hints.ai_socktype = SOCK_STREAM;

    std::string service = std::to_string(port);
    int ret = getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (ret != 0) {
        ZNET_LOG_ERROR("Address::lookup failed: host={}, port={}, error={}",
                       host, port, gai_strerror(ret));
        return result;
    }

    // 将链表中的每一项 sockaddr 包装成统一 Address 抽象。
    for (curr = res; curr != nullptr; curr = curr->ai_next) {
        Address::ptr addr = create(curr->ai_addr, curr->ai_addrlen);
        if (addr) {
            result.push_back(addr);
        }
    }

    freeaddrinfo(res);
    return result;
}

// 根据 sa_family 分派到具体地址类型，便于上层以多态方式处理。
Address::ptr Address::create(const sockaddr *addr, socklen_t addrlen) {
    (void)addrlen; // 参数保留用于未来扩展
    if (!addr) {
        return nullptr;
    }

    switch (addr->sa_family) {
    case AF_INET:
        return std::make_shared<IPv4Address>(
            *reinterpret_cast<const sockaddr_in *>(addr));
    case AF_INET6:
        return std::make_shared<IPv6Address>(
            *reinterpret_cast<const sockaddr_in6 *>(addr));
    case AF_UNIX:
        return std::make_shared<UnixAddress>(
            *reinterpret_cast<const sockaddr_un *>(addr));
    default:
        ZNET_LOG_ERROR("Address::create unknown address family: {}",
                       addr->sa_family);
        return nullptr;
    }
}

// IPv4 文本地址构造：非法地址会降级为 INADDR_ANY，避免构造阶段崩溃。
IPv4Address::IPv4Address(const std::string &ip, uint16_t port) {
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        ZNET_LOG_ERROR("IPv4Address::IPv4Address invalid ip: {}", ip);
        // 使用默认地址 0.0.0.0
        addr_.sin_addr.s_addr = INADDR_ANY;
    }
}

IPv4Address::IPv4Address(const sockaddr_in &addr) : addr_(addr) {}

// 统一输出为 ip:port 形式，便于日志检索和问题定位。
std::string IPv4Address::to_string() const {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));

    char out[INET_ADDRSTRLEN + 8];
    const int n = snprintf(out, sizeof(out), "%s:%u", buf,
                           static_cast<unsigned>(ntohs(addr_.sin_port)));
    if (n <= 0) {
        return std::string();
    }
    return std::string(out, static_cast<size_t>(n));
}

uint16_t IPv4Address::port() const { return ntohs(addr_.sin_port); }

void IPv4Address::set_port(uint16_t port) { addr_.sin_port = htons(port); }

// IPv6 文本地址构造：非法地址会降级为 in6addr_any。
IPv6Address::IPv6Address(const std::string &ip, uint16_t port) {
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin6_family = AF_INET6;
    addr_.sin6_port = htons(port);

    if (inet_pton(AF_INET6, ip.c_str(), &addr_.sin6_addr) <= 0) {
        ZNET_LOG_ERROR("IPv6Address::IPv6Address invalid ip: {}", ip);
        // 使用默认地址 ::
        addr_.sin6_addr = in6addr_any;
    }
}

IPv6Address::IPv6Address(const sockaddr_in6 &addr) : addr_(addr) {}

// 统一输出为 [ip]:port 形式，避免 IPv6 冒号与端口分隔冲突。
std::string IPv6Address::to_string() const {
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr_.sin6_addr, buf, sizeof(buf));

    char out[INET6_ADDRSTRLEN + 10];
    const int n = snprintf(out, sizeof(out), "[%s]:%u", buf,
                           static_cast<unsigned>(ntohs(addr_.sin6_port)));
    if (n <= 0) {
        return std::string();
    }
    return std::string(out, static_cast<size_t>(n));
}

uint16_t IPv6Address::port() const { return ntohs(addr_.sin6_port); }

void IPv6Address::set_port(uint16_t port) { addr_.sin6_port = htons(port); }

// Unix 域地址默认仅设置 family，路径由 set_path() 安全写入。
UnixAddress::UnixAddress(const std::string &path) {
    memset(&addr_, 0, sizeof(addr_));
    addr_.sun_family = AF_UNIX;

    if (!path.empty()) {
        set_path(path);
    }
}

UnixAddress::UnixAddress(const sockaddr_un &addr) : addr_(addr) {}

std::string UnixAddress::to_string() const {
    return std::string(addr_.sun_path);
}

// 路径过长时截断并告警，保证 sun_path 始终以 '\0' 终止。
void UnixAddress::set_path(const std::string &path) {
    size_t max_len = sizeof(addr_.sun_path) - 1;
    if (path.length() > max_len) {
        ZNET_LOG_WARN("UnixAddress::set_path path too long: {} > {}, truncated",
                      path.length(), max_len);
    }
    strncpy(addr_.sun_path, path.c_str(), max_len);
    addr_.sun_path[max_len] = '\0';
}

std::string UnixAddress::path() const { return std::string(addr_.sun_path); }

} // namespace znet
