#pragma once

#include "znet/error.h"
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <vector>

namespace znet {
// Validated, owning address value. Native access is const and borrowed.
class Endpoint {
  public:
    static Result<Endpoint> ipv4(std::string_view ip, uint16_t port = 0);
    static Result<Endpoint> ipv6(std::string_view ip, uint16_t port = 0);
    // Leading NUL selects Linux's abstract namespace. An empty path represents
    // an unnamed endpoint.
    static Result<Endpoint> unix_domain(std::string_view path);
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

// Blocking resolver: call during configuration, outside an IO worker.
Result<std::vector<Endpoint>> resolve_endpoints(std::string_view host,
                                                uint16_t port = 0,
                                                int family = AF_UNSPEC);
} // namespace znet
