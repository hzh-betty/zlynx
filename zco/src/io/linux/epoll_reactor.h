#pragma once
#include "io/reactor.h"
#include <map>
#include <mutex>

namespace zco {
namespace detail {
class EpollReactor final : public Reactor {
  public:
    EpollReactor();
    ~EpollReactor() override;
    Result<RegistrationId> add(io::ResourceId, int, io::Interest) override;
    Result<void> remove(RegistrationId) override;
    std::vector<ReadyEvent> poll(int timeout_ms) override;
    void wake() override;

  private:
    struct Bucket {
        int fd;
        std::map<RegistrationId, unsigned> registrations;
    };

    int epoll_ = -1, event_ = -1;
    std::mutex mutex_;
    std::map<uint64_t, Bucket> resources_;
    std::map<RegistrationId, uint64_t> registrations_;
};
} // namespace detail
} // namespace zco
