#include "znet/endpoint.h"

#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <sys/un.h>

namespace znet {
namespace {
Error invalid_address(std::string detail) {
    return make_error(ErrorKind::validation, std::errc::invalid_argument,
                      "endpoint", std::move(detail));
}

class ResolverCategory final : public std::error_category {
  public:
    const char *name() const noexcept override { return "resolver"; }

    std::string message(int code) const override {
        return ::gai_strerror(code);
    }
};
} // namespace

Result<Endpoint> Endpoint::ipv4(std::string_view ip, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (ip.find('\0') != std::string_view::npos ||
        ::inet_pton(AF_INET, std::string(ip).c_str(), &address.sin_addr) != 1)
        return invalid_address("invalid IPv4 address");
    return from_native(reinterpret_cast<const sockaddr *>(&address),
                       sizeof(address));
}

Result<Endpoint> Endpoint::ipv6(std::string_view ip, uint16_t port) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    if (ip.find('\0') != std::string_view::npos ||
        ::inet_pton(AF_INET6, std::string(ip).c_str(), &address.sin6_addr) != 1)
        return invalid_address("invalid IPv6 address");
    return from_native(reinterpret_cast<const sockaddr *>(&address),
                       sizeof(address));
}

Result<Endpoint> Endpoint::unix_domain(std::string_view path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const bool abstract = !path.empty() && path.front() == '\0';
    if (path.size() > sizeof(address.sun_path) - (abstract ? 0 : 1))
        return invalid_address("Unix path is too long");
    if (!abstract && path.find('\0') != std::string_view::npos)
        return invalid_address("embedded NUL in Unix path");
    if (!path.empty())
        std::memcpy(address.sun_path, path.data(), path.size());
    const auto size = offsetof(sockaddr_un, sun_path) + path.size() +
                      (!abstract && !path.empty() ? 1 : 0);
    return from_native(reinterpret_cast<const sockaddr *>(&address),
                       static_cast<socklen_t>(size));
}

Result<Endpoint> Endpoint::from_native(const sockaddr *address,
                                       socklen_t size) {
    if (!address || size < sizeof(sa_family_t) ||
        size > sizeof(sockaddr_storage))
        return invalid_address("invalid native address length");
    sa_family_t family;
    std::memcpy(&family, address, sizeof(family));
    socklen_t required;
    switch (family) {
    case AF_INET:
        required = sizeof(sockaddr_in);
        break;
    case AF_INET6:
        required = sizeof(sockaddr_in6);
        break;
    case AF_UNIX:
        required = offsetof(sockaddr_un, sun_path);
        if (size > sizeof(sockaddr_un))
            return invalid_address("invalid Unix address length");
        break;
    default:
        return invalid_address("unsupported address family");
    }
    if (size < required)
        return invalid_address("truncated native address");
    Endpoint endpoint;
    endpoint.size_ = family == AF_UNIX ? size : required;
    std::memcpy(&endpoint.storage_, address, endpoint.size_);
    return endpoint;
}

uint16_t Endpoint::port() const {
    if (family() == AF_INET)
        return ntohs(
            reinterpret_cast<const sockaddr_in *>(&storage_)->sin_port);
    if (family() == AF_INET6)
        return ntohs(
            reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_port);
    return 0;
}

std::string Endpoint::to_string() const {
    char text[INET6_ADDRSTRLEN]{};
    if (family() == AF_INET) {
        ::inet_ntop(AF_INET,
                    &reinterpret_cast<const sockaddr_in *>(&storage_)->sin_addr,
                    text, sizeof(text));
        return std::string(text) + ":" + std::to_string(port());
    }
    if (family() == AF_INET6) {
        ::inet_ntop(
            AF_INET6,
            &reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_addr, text,
            sizeof(text));
        return "[" + std::string(text) + "]:" + std::to_string(port());
    }
    const auto *address = reinterpret_cast<const sockaddr_un *>(&storage_);
    const size_t length = size_ - offsetof(sockaddr_un, sun_path);
    if (!length)
        return {};
    if (address->sun_path[0] == '\0')
        return "@" + std::string(address->sun_path + 1, length - 1);
    return std::string(address->sun_path, ::strnlen(address->sun_path, length));
}

Result<std::vector<Endpoint>> resolve_endpoints(std::string_view host,
                                                uint16_t port, int family) {
    if (host.empty() || host.find('\0') != std::string_view::npos)
        return invalid_address("invalid hostname");
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *raw = nullptr;
    const int rc = ::getaddrinfo(std::string(host).c_str(),
                                 std::to_string(port).c_str(), &hints, &raw);
    if (rc == EAI_SYSTEM)
        return io_error("resolve", errno);
    if (rc) {
        static const ResolverCategory category;
        return Error{ErrorKind::runtime, std::error_code(rc, category),
                     "resolve", std::string(host)};
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(
        raw, ::freeaddrinfo);
    std::vector<Endpoint> result;
    for (auto *entry = raw; entry; entry = entry->ai_next) {
        auto endpoint =
            Endpoint::from_native(entry->ai_addr, entry->ai_addrlen);
        if (endpoint)
            result.push_back(std::move(endpoint).value());
    }
    if (result.empty())
        return invalid_address("no supported addresses");
    return result;
}
} // namespace znet
