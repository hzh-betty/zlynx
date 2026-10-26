#ifndef ZLYNX_ZCO_SRC_IO_REACTOR_H_
#define ZLYNX_ZCO_SRC_IO_REACTOR_H_

#include "zco/io/operations.h"
#include <vector>

namespace zco {
namespace detail {
// 同一个资源可以同时有一个读等待和一个写等待，所以“资源”和“等待登记”不是一回事。
// 其次，资源关闭后，操作系统可能复用原来的 fd 编号；
// 项目给每个新资源分配递增的 ResourceId，避免仅凭 fd 把新旧资源混为一谈。
using RegistrationId = uint64_t; // 区分不同次 I/O 等待登记

struct ReadyEvent {
    RegistrationId registration;
    io::Interest interest;
};

class Reactor {
  public:
    virtual ~Reactor() = default;
    virtual Result<RegistrationId> add(io::ResourceId resource, int fd,
                                       io::Interest interest) = 0;
    virtual Result<void> remove(RegistrationId registration) = 0;
    virtual std::vector<ReadyEvent> poll(int timeout_ms) = 0;
    virtual void wake() = 0;
};
} // namespace detail
} // namespace zco

#endif
