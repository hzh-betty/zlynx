#ifndef ZLYNX_ZCO_SRC_IO_RESOURCE_H_
#define ZLYNX_ZCO_SRC_IO_RESOURCE_H_

#include "io/reactor.h"
#include "wait/wait_state.h"
#include <mutex>

namespace zco {
namespace io {
namespace detail {

/** 
 * @brief 描述一次在该资源上进行的等待，包括登记在哪个 Reactor 中，编号是多少，等待什么方向，对应哪个等待状态。 
 *
 */
struct Registration {
    std::weak_ptr<zco::detail::Reactor> reactor; // 找到负责监听的 Reactor，以便撤销登记
    zco::detail::RegistrationId id; // 反应器登记 ID；用于撤销登记
    std::weak_ptr<zco::detail::WaitState> wait; // 找到等待状态，以便在资源关闭时通知它
    Interest interest; //记录等待读、写还是读写，用于检查兴趣冲突
};

/**
 * @brief 描述符的共享控制状态，协调系统调用借用、等待登记和关闭。
 *
 * mutex 保护 fd 及 registrations；id 区分资源实例与可能被系统复用的 fd。
 * 注意 Resource 对象仍然存在，不代表文件描述符仍然打开
 */
struct Resource {
    explicit Resource(int descriptor);
    std::mutex mutex;

    // -1 表示已关闭；即使控制状态仍被等待登记持有，也不能再借用该描述符。
    int fd;
    ResourceId id; // 资源实例 ID；用于区分可能被系统复用的 fd。
    const bool nonblocking; // 记录接管时的状态，外部不得通过别名改变对应的非阻塞标志。
    std::vector<std::weak_ptr<Registration>> registrations; // 该资源上所有登记的等待
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
