#pragma once
#include "zco/io/operations.h"
#include <vector>

namespace zco {
namespace detail {
using RegistrationId = uint64_t;

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
