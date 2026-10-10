#pragma once
#include "io/reactor.h"
#include "wait/wait_state.h"
#include <mutex>

namespace zco {
namespace io {
namespace detail {
struct Registration {
    std::weak_ptr<zco::detail::Reactor> reactor;
    zco::detail::RegistrationId id;
    std::weak_ptr<zco::detail::WaitState> wait;
    Interest interest;
};

struct Resource {
    explicit Resource(int descriptor);
    std::mutex mutex;
    int fd;
    ResourceId id;
    const bool nonblocking;
    std::vector<std::weak_ptr<Registration>> registrations;
};
} // namespace detail
} // namespace io
} // namespace zco

namespace zco {
namespace detail {
Result<RegistrationId>
register_io(const std::shared_ptr<Reactor> &,
            const std::shared_ptr<WaitState> &,
            const std::shared_ptr<io::detail::Resource> &, io::Interest,
            std::shared_ptr<io::detail::Registration> &);
Result<void> unregister_io(const std::shared_ptr<Reactor> &,
                           const std::shared_ptr<io::detail::Resource> &,
                           const std::shared_ptr<io::detail::Registration> &);
Result<void> current_wait_ready(const std::shared_ptr<io::detail::Resource> &,
                                io::Interest, Deadline);
} // namespace detail
} // namespace zco
