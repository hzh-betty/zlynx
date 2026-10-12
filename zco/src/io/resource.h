#ifndef ZLYNX_ZCO_SRC_IO_RESOURCE_H_
#define ZLYNX_ZCO_SRC_IO_RESOURCE_H_

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

/**
 * @brief 描述符的共享控制状态，协调系统调用借用、等待登记和关闭。
 *
 * mutex 保护 fd 及 registrations；id 区分资源实例与可能被系统复用的 fd。
 * nonblocking 记录接管时的状态，外部不得通过别名改变对应的非阻塞标志。
 */
struct Resource {
    explicit Resource(int descriptor);
    std::mutex mutex;

    // -1 表示已关闭；即使控制状态仍被等待登记持有，也不能再借用该描述符。
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

#endif
